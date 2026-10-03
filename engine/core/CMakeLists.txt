# engine_core: defines, memory, templates, utils and the ECS. This module has no
# external dependencies on purpose: tests and benchmarks link it alone, so it
# must stay free of graphics / platform code.

file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)
file(GLOB_RECURSE CORE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
)

add_library(engine_core STATIC ${CORE_SOURCES} ${CORE_HEADERS})
add_library(LuauEngine::core ALIAS engine_core)

target_include_directories(engine_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(engine_core PUBLIC cxx_std_20)
