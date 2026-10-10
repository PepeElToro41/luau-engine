# engine_tests: every *_tests.cpp under this directory, one doctest binary.
# Links only LuauEngine::core so it builds without SDL3 / Vulkan. The one
# graphics source compiled in is the shader preprocessing step, which is
# plain text processing over core headers (its header pulls in no Vulkan).

set(GRAPHICS_DIR ${PROJECT_SOURCE_DIR}/engine/graphics)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
)

add_executable(engine_tests
    ${TEST_SOURCES}
    ${GRAPHICS_DIR}/src/shaders/preprocessing.cpp
)

target_include_directories(engine_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GRAPHICS_DIR}/include
)
target_link_libraries(engine_tests
    PRIVATE
        LuauEngine::core
        doctest::doctest
)

# One CTest entry per TEST_CASE, so `ctest --test-dir build` works too.
include(${PROJECT_SOURCE_DIR}/vendor/doctest/doctest.cmake)
doctest_discover_tests(engine_tests)
