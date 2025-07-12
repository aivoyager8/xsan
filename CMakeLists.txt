# 设置 SPDK 相关库变量，包含 uuid 支持
set(XSAN_SPDK_LIBRARIES spdk_util spdk_bdev spdk_env_dpdk uuid)
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

# Option to enable SPDK support
option(XSAN_ENABLE_SPDK "Enable SPDK integration" ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # Good for IDEs






# 查找系统 rocksdb 头文件和库
find_path(ROCKSDB_INCLUDE_DIR NAMES rocksdb/c.h PATHS /usr/include /usr/local/include)
find_library(ROCKSDB_LIBRARY NAMES rocksdb PATHS /usr/lib /usr/local/lib)
if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
    message(STATUS "Found system RocksDB include: ${ROCKSDB_INCLUDE_DIR}")
    message(STATUS "Found system RocksDB library: ${ROCKSDB_LIBRARY}")
    include_directories(SYSTEM ${ROCKSDB_INCLUDE_DIR})
    set(XSAN_ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARY} CACHE INTERNAL "RocksDB Libraries")
else()
    message(FATAL_ERROR "System RocksDB not found. Please install librocksdb-dev.")
endif()

# 查找并集成 NUMA 库
find_path(NUMA_INCLUDE_DIR NAMES numa.h PATHS /usr/include /usr/local/include)
find_library(NUMA_LIBRARY NAMES numa PATHS /usr/lib /usr/local/lib)
if(NUMA_INCLUDE_DIR AND NUMA_LIBRARY)
    message(STATUS "Found NUMA include: ${NUMA_INCLUDE_DIR}")
    message(STATUS "Found NUMA library: ${NUMA_LIBRARY}")
    include_directories(SYSTEM ${NUMA_INCLUDE_DIR})
    set(XSAN_NUMA_LIBRARIES ${NUMA_LIBRARY} CACHE INTERNAL "NUMA Libraries")
else()
    message(WARNING "NUMA library not found, performance may be affected.")
endif()


# 构建 SPDK 子模块（假设使用 make，需提前配置好 SPDK 环境）
add_custom_target(MakeSPDK
    COMMAND make -C ${CMAKE_SOURCE_DIR}/src/spdk
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/src/spdk
    COMMENT "Building SPDK submodule"
)


# xsan 主包依赖 SPDK 构建（RocksDB 由系统提供，无需自定义构建目标）
add_custom_target(xsan_all DEPENDS MakeSPDK
    COMMENT "Build SPDK and XSAN together (RocksDB from system)"
)

# SPDK 头文件和库路径（需先在 src/spdk 编译生成 build/lib 和 include）
include_directories(src/spdk/include)
link_directories(src/spdk/build/lib)


# 添加头文件路径（仅 SPDK，RocksDB 由系统提供）
include_directories(src/spdk/include)

# 其他依赖（如 json-c）仍可用 pkg-config 查找
find_package(PkgConfig REQUIRED)
pkg_search_module(JSON_C REQUIRED json-c)
if(JSON_C_FOUND)
    message(STATUS "Found json-c: ${JSON_C_LIBRARIES}")
    message(STATUS "json-c Include Dirs: ${JSON_C_INCLUDE_DIRS}")
    set(XSAN_JSON_C_INCLUDE_DIRS ${JSON_C_INCLUDE_DIRS} CACHE INTERNAL "json-c Include Directories")
    set(XSAN_JSON_C_LIBRARIES ${JSON_C_LIBRARIES} CACHE INTERNAL "json-c Libraries")
    set(XSAN_JSON_C_LIBRARY_DIRS ${JSON_C_LIBRARY_DIRS} CACHE INTERNAL "json-c Library Directories")
    include_directories(SYSTEM ${XSAN_JSON_C_INCLUDE_DIRS})
else()
    message(FATAL_ERROR "json-c library not found by pkg-config. Please install json-c.")
endif()

# Add all relevant source directories now
add_subdirectory(src/utils)
add_subdirectory(src/common)
add_subdirectory(src/network) # Added network
add_subdirectory(src/core)    # Added core (for spdk_manager)
add_subdirectory(src/bdev)    # Added bdev
add_subdirectory(src/io)      # Added io (for xsan_io)
add_subdirectory(src/replication) # Added replication (for replicated IO contexts etc.)
add_subdirectory(src/metadata) # Added metadata (for RocksDB store wrapper)
add_subdirectory(src/storage) # Added storage (for disk_manager, volume_manager)
add_subdirectory(src/vhost)   # Added vhost (for XSAN vbdev and vhost integration)
add_subdirectory(src/nvmf)    # Added NVMe-oF target module
add_subdirectory(src/main)    # Main executable likely defined here
add_subdirectory(tests)

# Display configuration information
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
