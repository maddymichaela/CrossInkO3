add_executable(Ao3SyncUrlsTest
  Ao3SyncUrlsTest.cpp
)

target_include_directories(Ao3SyncUrlsTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(Ao3SyncUrlsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Ao3SyncUrlsTest)
