cmake_minimum_required(VERSION 3.24)

project(RNDFollowers VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------------------------
# CommonLibSSE-NG (alandtse "ng" fork). The old colorglass vcpkg port (3.7.0) cannot read the
# Address Library format 5 used by Skyrim 1.7.99 / 1.7.104, so the library is now built from
# source. The GitHub workflow clones it into extern/CommonLibSSE-NG (a git submodule works too).
# NOTE: this library is GPL-3.0-or-later (with a modding exception); see README/LICENSE.
# ---------------------------------------------------------------------------------------------
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)           # tests need Catch2, we do not need them
set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
add_subdirectory(extern/CommonLibSSE-NG)

# add_commonlibsse_plugin() normally comes with find_package(CommonLibSSE); when the library is
# added as a subdirectory, load the helper by hand if it is not available yet.
if(NOT COMMAND add_commonlibsse_plugin)
    include(${CMAKE_CURRENT_SOURCE_DIR}/extern/CommonLibSSE-NG/cmake/CommonLibSSE.cmake)
endif()

find_package(spdlog CONFIG REQUIRED)
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h" REQUIRED)

# Builds a version-independent SKSE plugin (SE / AE / 1.7.x) that declares Address Library v5 support.
add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "RNDFollowers"
    USE_ADDRESS_LIBRARY
    SOURCES src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${SIMPLEINI_INCLUDE_DIRS} ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)

# main.cpp contains UTF-8 text (Chinese menu labels); make MSVC read sources as UTF-8
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
