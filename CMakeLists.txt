cmake_minimum_required(VERSION 3.10)
project(IsolateGalgame)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_AUTOMOC ON) # Qt 
include_directories(${CMAKE_SOURCE_DIR}/include)
find_package(Qt5 COMPONENTS Widgets Concurrent REQUIRED)
# 终端
add_executable(galgame src/main.cpp)
target_link_libraries(galgame sqlite3 curl)
# Qt 图形界面
add_executable(qt_client src/qt_main.cpp)
target_link_libraries(qt_client Qt5::Widgets Qt5::Concurrent sqlite3 curl)