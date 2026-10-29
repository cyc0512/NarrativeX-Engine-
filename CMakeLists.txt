cmake_minimum_required(VERSION 3.10)
# 👇 引擎名
project(NarrativeX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_AUTOMOC ON)
include_directories(${CMAKE_SOURCE_DIR}/include)
find_package(Qt5 COMPONENTS Widgets Concurrent REQUIRED)
# 终端测试版
add_executable(nx_terminal src/main.cpp)
target_link_libraries(nx_terminal sqlite3 curl)

add_executable(fluidstory src/qt_main.cpp)
target_link_libraries(fluidstory Qt5::Widgets Qt5::Concurrent sqlite3 curl)