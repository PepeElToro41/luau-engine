# nanobench, vendored single header. Sourced from
# https://github.com/martinus/nanobench, tag v4.6.0.

add_library(nanobench INTERFACE)
add_library(nanobench::nanobench ALIAS nanobench)

target_include_directories(nanobench INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
