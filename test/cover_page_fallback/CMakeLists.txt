add_executable(CoverPageFallbackTest
  CoverPageFallbackTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CoverPageFallback.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(CoverPageFallbackTest PRIVATE
  ${REPO_ROOT}/test/fs_helpers/stubs
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(CoverPageFallbackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CoverPageFallbackTest)
