add_executable(WaveformActionsTest
  WaveformActionsTest.cpp
  ${REPO_ROOT}/lib/WaveformActions/WaveformActions.cpp
)

target_include_directories(WaveformActionsTest PRIVATE
  ${REPO_ROOT}/lib/WaveformActions
)

target_link_libraries(WaveformActionsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WaveformActionsTest)
