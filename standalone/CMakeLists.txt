file(GLOB_RECURSE STANDALONE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

add_executable(standalone ${STANDALONE_SOURCES})

target_include_directories(standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(standalone PRIVATE LuauEngine::engine)
