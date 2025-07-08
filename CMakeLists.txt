cmake_minimum_required(VERSION 3.16)
project(xsan VERSION 1.0.0 LANGUAGES C)

# 设置 C 标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# 全局包含目录
include_directories(include)
include_directories(src/include)

# 线程库
find_package(Threads REQUIRED)

# 只构建基础模块
add_subdirectory(src/utils)
add_subdirectory(src/common)
add_subdirectory(tests)

# 显示配置信息
message(STATUS "")
message(STATUS "=== XSAN Configuration (Simplified) ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "C Flags: ${CMAKE_C_FLAGS}")
message(STATUS "")
message(STATUS "Building basic modules only:")
message(STATUS "  - Utils module: ON")
message(STATUS "  - Common module: ON")
message(STATUS "  - Tests: ON")
message(STATUS "=======================================")
message(STATUS "")
