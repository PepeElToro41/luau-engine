# engine_bench: every *_bench.cpp under this directory, one nanobench binary.
# Links only LuauEngine::core. Build it in a Release configuration; a Debug
# build runs but prints a warning since ENGINE_ASSERT and -O0 skew the numbers.

file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

add_executable(engine_bench ${BENCH_SOURCES})

target_include_directories(engine_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine_bench
    PRIVATE
        LuauEngine::core
        nanobench::nanobench
)
