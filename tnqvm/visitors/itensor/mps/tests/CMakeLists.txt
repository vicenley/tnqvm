find_package(GTest REQUIRED)

add_xacc_test(ITensorMPSVisitor)
#find_package(GTest)
include_directories(${GTEST_INCLUDE_DIRS})
#target_link_libraries(ITensorMPSVisitorTester tnqvm-itensor)

target_link_libraries(ITensorMPSVisitorTester tnqvm-itensor GTest::GTest GTest::Main)
#target_link_libraries(ITensorMPSVisitorTester PRIVATE GTest::GTest GTest::Main)