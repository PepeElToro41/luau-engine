# engine_tests: every *_tests.cpp under this directory, one doctest binary.
# Links only LuauEngine::core so it builds without SDL3 / Vulkan.

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

add_executable(engine_tests ${TEST_SOURCES})

target_include_directories(engine_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine_tests
    PRIVATE
        LuauEngine::core
        doctest::doctest
)

# One CTest entry per TEST_CASE, so `ctest --test-dir build` works too.
include(${PROJECT_SOURCE_DIR}/vendor/doctest/doctest.cmake)
doctest_discover_tests(engine_tests)
