# doctest, vendored single header. Sourced from https://github.com/doctest/doctest,
# tag v2.5.3. doctest.cmake / doctestAddTests.cmake are the upstream CTest
# integration scripts (doctest_discover_tests).

add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)

target_include_directories(doctest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
