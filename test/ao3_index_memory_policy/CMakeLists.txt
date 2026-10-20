add_executable(Ao3IndexMemoryPolicyTest
  Ao3IndexMemoryPolicyTest.cpp
)

target_include_directories(Ao3IndexMemoryPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(Ao3IndexMemoryPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Ao3IndexMemoryPolicyTest)
