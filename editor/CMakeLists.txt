file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_executable(editor ${EDITOR_SOURCES})

target_include_directories(editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Stand-in for an open-project page: the editor opens this directory as the
# current project at startup so the Asset Browser has something to show.
target_compile_definitions(editor PRIVATE EDITOR_TEST_PROJECT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testing_project")
target_link_libraries(editor PRIVATE LuauEngine::engine stb::image)
