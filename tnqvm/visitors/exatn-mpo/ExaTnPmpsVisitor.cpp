#include "ExaTnPmpsVisitor.hpp"
#include "tensor_basic.hpp"
#include "talshxx.hpp"
#include "utils/GateMatrixAlgebra.hpp"
#include <map>
#ifdef TNQVM_EXATN_USES_MKL_BLAS
#include <dlfcn.h>
#endif

#define INITIAL_BOND_DIM 1
#define INITIAL_KRAUS_DIM 1
#define QUBIT_DIM 2

namespace {
std::vector<std::complex<double>> Q_ZERO_TENSOR_BODY(size_t in_volume)
{
    std::vector<std::complex<double>> body(in_volume, {0.0, 0.0});
    body[0] = std::complex<double>(1.0, 0.0);
    return body;
} 

const std::string ROOT_TENSOR_NAME = "Root";
}
namespace tnqvm {
ExaTnPmpsVisitor::ExaTnPmpsVisitor()
{
    // TODO
}

void ExaTnPmpsVisitor::initialize(std::shared_ptr<AcceleratorBuffer> buffer, int nbShots) 
{ 
    // Initialize ExaTN (if not already initialized)
    if (!exatn::isInitialized()) 
    {
#ifdef TNQVM_EXATN_USES_MKL_BLAS
        // Fix for TNQVM bug #30
        void *core_handle =
            dlopen("@EXATN_MKL_PATH@/libmkl_core@CMAKE_SHARED_LIBRARY_SUFFIX@",
                    RTLD_LAZY | RTLD_GLOBAL);
        if (core_handle == nullptr) {
            std::string err = std::string(dlerror());
            xacc::error("Could not load mkl_core - " + err);
        }

        void *thread_handle = dlopen(
            "@EXATN_MKL_PATH@/libmkl_gnu_thread@CMAKE_SHARED_LIBRARY_SUFFIX@",
            RTLD_LAZY | RTLD_GLOBAL);
        if (thread_handle == nullptr) {
            std::string err = std::string(dlerror());
            xacc::error("Could not load mkl_gnu_thread - " + err);
        }
#endif
        // If exaTN has not been initialized, do it now.
        exatn::initialize();
        // ExaTN and XACC logging levels are always in-synced.
        exatn::resetRuntimeLoggingLevel(xacc::verbose ? xacc::getLoggingLevel() : 0);
        xacc::subscribeLoggingLevel([](int level) {
            exatn::resetRuntimeLoggingLevel(xacc::verbose ? level : 0);
        });
    }

    m_buffer = buffer;
    m_pmpsTensorNetwork = buildInitialNetwork(buffer->size());
}

exatn::TensorNetwork ExaTnPmpsVisitor::buildInitialNetwork(size_t in_nbQubits) const
{   
    for (int i = 0; i < in_nbQubits; ++i)
    {
        const std::string tensorName = "Q" + std::to_string(i);
        auto tensor = [&](){ 
            if (in_nbQubits == 1)
            {
                assert(tensorName == "Q0");
                return std::make_shared<exatn::Tensor>(tensorName, exatn::TensorShape{QUBIT_DIM, INITIAL_BOND_DIM}); 
            } 
            if (i == 0) 
            {
                return std::make_shared<exatn::Tensor>(tensorName, exatn::TensorShape{QUBIT_DIM, INITIAL_BOND_DIM, INITIAL_KRAUS_DIM}); 
            }
            if (i == (in_nbQubits - 1))
            {
                return std::make_shared<exatn::Tensor>(tensorName, exatn::TensorShape{INITIAL_BOND_DIM, QUBIT_DIM, INITIAL_KRAUS_DIM}); 
            }
            return std::make_shared<exatn::Tensor>(tensorName, exatn::TensorShape{INITIAL_BOND_DIM, QUBIT_DIM, INITIAL_BOND_DIM, INITIAL_KRAUS_DIM});
        }();

        const bool created = exatn::createTensorSync(tensor, exatn::TensorElementType::COMPLEX64);
        assert(created);
        const bool initialized = exatn::initTensorDataSync(tensorName, Q_ZERO_TENSOR_BODY(tensor->getVolume()));
        assert(initialized);
    }
    
    const auto buildTensorMap = [](size_t in_nbQubits) {
        const std::vector<int> qubitTensorDim(in_nbQubits, QUBIT_DIM);
        const std::vector<int> ancTensorDim(in_nbQubits, INITIAL_KRAUS_DIM);
        // Root tensor dimension: 2 .. 2 (upper legs/system dimensions) 1 ... 1 (lower legs/anc dimension)
        std::vector<int> rootTensorDim;
        rootTensorDim.insert(rootTensorDim.end(), qubitTensorDim.begin(), qubitTensorDim.end());
        rootTensorDim.insert(rootTensorDim.end(), ancTensorDim.begin(), ancTensorDim.end());
        auto rootTensor = std::make_shared<exatn::Tensor>(ROOT_TENSOR_NAME, qubitTensorDim);
        std::map<std::string, std::shared_ptr<exatn::Tensor>> tensorMap;
        tensorMap.emplace(ROOT_TENSOR_NAME, rootTensor);
        for (int i = 0; i < in_nbQubits; ++i)
        {
            const std::string qTensorName = "Q" + std::to_string(i);
            tensorMap.emplace(qTensorName, exatn::getTensor(qTensorName));
        }
        return tensorMap;
    };

    const std::string rootVarNameList = [](size_t in_nbQubits){
        std::string result = "(";
        // Primary qubit legs
        for (int i = 0; i < in_nbQubits; ++i)
        {
            result += ("i" + std::to_string(i) + ",");
        }

        // Anc legs:
        for (int i = 0; i < in_nbQubits; ++i)
        {
            result += ("k" + std::to_string(i) + ",");
        }
        assert(result.back() == ',');
        result.back() = ')';
        return result;
    }(in_nbQubits);
    
    const auto qubitTensorVarNameList = [](int in_qIdx, int in_nbQubits) -> std::string {
        if (in_qIdx == 0)
        {
            return "(i0,j0,k0)";
        } 
        if (in_qIdx == in_nbQubits - 1)
        {
            return "(j" + std::to_string(in_nbQubits - 2) + ",i" + std::to_string(in_nbQubits - 1) + ",k" + std::to_string(in_nbQubits - 1) + ")";
        }

        return "(j" + std::to_string(in_qIdx-1) + ",i" + std::to_string(in_qIdx) + ",j" + std::to_string(in_qIdx) + ",k" + std::to_string(in_qIdx) + ")";
    };

    const std::string pmpsString = [&]() {
        std::string result = ROOT_TENSOR_NAME + rootVarNameList + "=";
        for (int i = 0; i < in_nbQubits - 1; ++i)
        {
            result += ("Q" + std::to_string(i) + qubitTensorVarNameList(i, in_nbQubits) + "*");
        }
        result += ("Q" + std::to_string(m_buffer->size() - 1) + qubitTensorVarNameList(in_nbQubits - 1, in_nbQubits));
        return result;
    }();

    std::cout << "Purified MPS: \n" << pmpsString << "\n";
    exatn::TensorNetwork purifiedMps("PMPS_Network", pmpsString, buildTensorMap(in_nbQubits));
    purifiedMps.printIt();
    return purifiedMps;
}

void ExaTnPmpsVisitor::finalize() 
{ 
    // TODO
}

void ExaTnPmpsVisitor::visit(Identity& in_IdentityGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(Hadamard& in_HadamardGate) 
{ 
   
}

void ExaTnPmpsVisitor::visit(X& in_XGate) 
{ 
   
}

void ExaTnPmpsVisitor::visit(Y& in_YGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(Z& in_ZGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(Rx& in_RxGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(Ry& in_RyGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(Rz& in_RzGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(T& in_TGate) 
{ 
   
}

void ExaTnPmpsVisitor::visit(Tdg& in_TdgGate) 
{ 
   
}

// others
void ExaTnPmpsVisitor::visit(Measure& in_MeasureGate) 
{ 
}

void ExaTnPmpsVisitor::visit(U& in_UGate) 
{ 
    
}

// two-qubit gates: 
void ExaTnPmpsVisitor::visit(CNOT& in_CNOTGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(Swap& in_SwapGate) 
{ 
    
}

void ExaTnPmpsVisitor::visit(CZ& in_CZGate) 
{ 
   
}

void ExaTnPmpsVisitor::visit(CPhase& in_CPhaseGate) 
{ 
   
}

void ExaTnPmpsVisitor::visit(iSwap& in_iSwapGate) 
{
    
}

void ExaTnPmpsVisitor::visit(fSim& in_fsimGate) 
{
   
}

const double ExaTnPmpsVisitor::getExpectationValueZ(std::shared_ptr<CompositeInstruction> in_function) 
{ 
    return 0.0;
}
}
