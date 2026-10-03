# engine_runtime: the Engine application object that ties the modules together.
# Later home of physics and other simulation systems.

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)
file(GLOB_RECURSE RUNTIME_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

add_library(engine_runtime STATIC ${RUNTIME_SOURCES} ${RUNTIME_HEADERS})
add_library(LuauEngine::runtime ALIAS engine_runtime)

target_include_directories(engine_runtime
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(engine_runtime PUBLIC cxx_std_20)

target_link_libraries(engine_runtime
    PUBLIC
        LuauEngine::core
        LuauEngine::graphics
)
