# 指定cmake最低版本
cmake_minimum_required(VERSION 3.0)

# 头文件搜索路径
include_directories(./include)

# 可执行文件输出路径
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

# 设置g++编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -std=c++14")

# 指定生成的可执行文件
add_executable(test ./test/test.cpp)