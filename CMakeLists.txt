cmake_minimum_required(VERSION 3.10)
# 👇 引擎名
project(FS)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 命令行传入的 -DCMAKE_BUILD_TYPE 优先；没传才兜底为 Debug（gdb 需要 -g）。
# 注意：不要用无条件的 set()，否则会覆盖命令行传入的值。
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()
set(CMAKE_AUTOMOC ON)
# 三条 include 路径：现有 Qt 工程头 / nx 内核头 / 持久化头。
# 三者零重名（engine/include 只有 nx.hpp + nx/*.hpp，persistence/include 只有 nx/EventStore.hpp，
# 现有 include/ 下既无 nx.hpp 也无 nx/ 子目录），同时挂载不会产生解析歧义。
# T04 的 src/engine/GameSession.cpp 需要 #include "nx.hpp"，故此处必须提前就位。
include_directories(
  ${CMAKE_SOURCE_DIR}/src 
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/engine/include
  ${CMAKE_SOURCE_DIR}/persistence/include
)
find_package(Qt5 COMPONENTS Widgets Concurrent REQUIRED)
# 终端测试版
add_executable(fs_terminal src/main.cpp src/AiSelector.cpp src/EngineAdapter.cpp)
target_link_libraries(fs_terminal sqlite3 curl)

add_executable(fs src/qt_main.cpp src/AiSelector.cpp src/EngineAdapter.cpp)
target_link_libraries(fs Qt5::Widgets Qt5::Concurrent sqlite3 curl)