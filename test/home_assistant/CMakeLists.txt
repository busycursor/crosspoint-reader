add_executable(HomeAssistantTest
  HaConfigTest.cpp
  HaDashboardTest.cpp
  HaStateTest.cpp
  HaIdlePolicyTest.cpp
  ${REPO_ROOT}/lib/HomeAssistant/HaIdlePolicy.cpp
  ${REPO_ROOT}/lib/HomeAssistant/HaState.cpp
  ${REPO_ROOT}/lib/HomeAssistant/HaDashboard.cpp
  ${REPO_ROOT}/lib/HomeAssistant/HaConfig.cpp
)

target_include_directories(HomeAssistantTest PRIVATE
  ${REPO_ROOT}/lib/HomeAssistant
)

target_link_libraries(HomeAssistantTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(HomeAssistantTest)
