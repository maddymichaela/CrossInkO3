add_executable(Ao3SyncMemoryPolicyTest
  Ao3SyncMemoryPolicyTest.cpp
)

target_include_directories(Ao3SyncMemoryPolicyTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(Ao3SyncMemoryPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Ao3SyncMemoryPolicyTest)
