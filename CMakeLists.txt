cmake_minimum_required(VERSION 3.24)

project(RNDFollowers VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h" REQUIRED)

# Builds a version-independent SKSE plugin (SE/AE) with NG's helper.
add_commonlibsse_plugin(${PROJECT_NAME}
    AUTHOR "RNDFollowers"
    SOURCES src/main.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE ${SIMPLEINI_INCLUDE_DIRS})
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
