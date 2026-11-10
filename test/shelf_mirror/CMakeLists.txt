add_executable(ShelfMirrorTest
  ShelfMirrorTest.cpp
  ${REPO_ROOT}/lib/ShelfMirror/ShelfMirror.cpp
)

target_include_directories(ShelfMirrorTest PRIVATE
  ${REPO_ROOT}/lib/ShelfMirror
)

target_link_libraries(ShelfMirrorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ShelfMirrorTest)
