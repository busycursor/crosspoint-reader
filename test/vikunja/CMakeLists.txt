add_executable(VikunjaTest
  VikunjaConfigTest.cpp
  VikunjaModelTest.cpp
  VikunjaPendingTest.cpp
  VikunjaStatusTest.cpp
  ${REPO_ROOT}/lib/Vikunja/VikunjaConfig.cpp
  ${REPO_ROOT}/lib/Vikunja/VikunjaModel.cpp
  ${REPO_ROOT}/lib/Vikunja/VikunjaPending.cpp
  ${REPO_ROOT}/lib/Vikunja/VikunjaStatus.cpp
)

target_include_directories(VikunjaTest PRIVATE
  ${REPO_ROOT}/lib/Vikunja
)

target_compile_definitions(VikunjaTest PRIVATE
  VIKUNJA_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(VikunjaTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(VikunjaTest)
