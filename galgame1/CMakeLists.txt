cmake_minimum_required(VERSION 3.10)
project(IsolateGalgame)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Debug)

# 告诉 CMake，源码在 src 文件夹里！
add_executable(galgame src/simple_galgame.cpp)