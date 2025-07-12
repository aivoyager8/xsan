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

if(XSAN_ENABLE_SPDK)
    message(STATUS "SPDK support is enabled for XSAN.")
    find_package(PkgConfig REQUIRED)

    set(SPDK_REQUIRED_MODULES
        spdk_env_dpdk
        spdk_log
        spdk_util
        spdk_thread
        spdk_event      # Core event library (for spdk_app_*, sock_group polling)
        spdk_net        # For spdk_net_framework and potentially sock implementations
        spdk_bdev
        spdk_scsi       # For vhost-scsi target implementation
        spdk_vhost      # For vhost library itself
    )
    # Optional: spdk_rpc, spdk_jsonrpc, spdk_nvmf, spdk_sock_posix (if explicit linking needed)

    set(XSAN_SPDK_INCLUDE_DIRS_INIT "")
    set(XSAN_SPDK_LIBRARIES_INIT "")
    set(XSAN_SPDK_LIBRARY_DIRS_INIT "")
    set(XSAN_SPDK_CFLAGS_INIT "")

    foreach(module ${SPDK_REQUIRED_MODULES})
        pkg_search_module(SPDK_MODULE_${module} REQUIRED ${module}) # Use unique prefix per module
        if(SPDK_MODULE_${module}_FOUND)
            message(STATUS "Found SPDK module: ${module}")
            list(APPEND XSAN_SPDK_INCLUDE_DIRS_INIT ${SPDK_MODULE_${module}_INCLUDE_DIRS})
            list(APPEND XSAN_SPDK_LIBRARIES_INIT ${SPDK_MODULE_${module}_LIBRARIES})
            # Some .pc files provide LDFLAGS which include -L and -l, others just LIBRARIES with full paths or -l
            # PKG_CONFIG_PATH needs to be set correctly for pkg-config to find .pc files.
            # If LIBRARIES are full paths, LIBRARY_DIRS might not be strictly needed for linking
            # but can be useful for rpath or if LIBRARIES are just -lname.
            list(APPEND XSAN_SPDK_LIBRARY_DIRS_INIT ${SPDK_MODULE_${module}_LIBRARY_DIRS})
            list(APPEND XSAN_SPDK_CFLAGS_INIT ${SPDK_MODULE_${module}_CFLAGS_OTHER})
        else()
            message(FATAL_ERROR "Required SPDK module '${module}' not found by pkg-config. Check PKG_CONFIG_PATH.")
        endif()
    endforeach()

    list(REMOVE_DUPLICATES XSAN_SPDK_INCLUDE_DIRS_INIT)
    list(REMOVE_DUPLICATES XSAN_SPDK_LIBRARIES_INIT)
    list(REMOVE_DUPLICATES XSAN_SPDK_LIBRARY_DIRS_INIT)
    list(REMOVE_DUPLICATES XSAN_SPDK_CFLAGS_INIT)

    # Set cache variables to pass to subdirectories, especially src/main/CMakeLists.txt
    set(XSAN_SPDK_INCLUDE_DIRS ${XSAN_SPDK_INCLUDE_DIRS_INIT} CACHE INTERNAL "Aggregated SPDK Include Directories")
    set(XSAN_SPDK_LIBRARIES ${XSAN_SPDK_LIBRARIES_INIT} CACHE INTERNAL "Aggregated SPDK Libraries")
    set(XSAN_SPDK_LIBRARY_DIRS ${XSAN_SPDK_LIBRARY_DIRS_INIT} CACHE INTERNAL "Aggregated SPDK Library Directories")
    set(XSAN_SPDK_CFLAGS ${XSAN_SPDK_CFLAGS_INIT} CACHE INTERNAL "Aggregated SPDK Compile Flags")

    message(STATUS "SPDK Include Dirs: ${XSAN_SPDK_INCLUDE_DIRS}")
    message(STATUS "SPDK Libraries: ${XSAN_SPDK_LIBRARIES}")
    message(STATUS "SPDK Library Dirs: ${XSAN_SPDK_LIBRARY_DIRS}")
    message(STATUS "SPDK CFLAGS: ${XSAN_SPDK_CFLAGS}")

    # Apply CFLAGS and Include Dirs globally for simplicity, or apply per-target
    include_directories(SYSTEM ${XSAN_SPDK_INCLUDE_DIRS})
    add_compile_options(${XSAN_SPDK_CFLAGS})

    # --- Find RocksDB (for C API) ---
    pkg_search_module(ROCKSDB REQUIRED rocksdb)
    if(ROCKSDB_FOUND)
        message(STATUS "Found RocksDB: ${ROCKSDB_LIBRARIES}")
        message(STATUS "RocksDB Include Dirs: ${ROCKSDB_INCLUDE_DIRS}")
        # RocksDB C API typically just needs the main include dir.
        # The library itself might be C++, so C++ linker deps might be needed.
        set(XSAN_ROCKSDB_INCLUDE_DIRS ${ROCKSDB_INCLUDE_DIRS} CACHE INTERNAL "RocksDB Include Directories")
        set(XSAN_ROCKSDB_LIBRARIES ${ROCKSDB_LIBRARIES} CACHE INTERNAL "RocksDB Libraries")
        set(XSAN_ROCKSDB_LIBRARY_DIRS ${ROCKSDB_LIBRARY_DIRS} CACHE INTERNAL "RocksDB Library Directories")
        include_directories(SYSTEM ${XSAN_ROCKSDB_INCLUDE_DIRS})
        # RocksDB often requires linking against stdc++ if its C API is a wrapper around C++ core
        # This will be handled at the target_link_libraries for the executable or specific lib using it.
    else()
        message(FATAL_ERROR "RocksDB (C API) not found by pkg-config. Please install RocksDB with C API support.")
    endif()

    # --- Find json-c ---
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

else()
    message(STATUS "SPDK support is disabled for XSAN.")
    message(STATUS "RocksDB and json-c integration will also be skipped as they are primarily for SPDK-based metadata.")
    # Ensure variables are defined as empty if SPDK is off, so src/main/CMakeLists.txt doesn't break
    set(XSAN_SPDK_INCLUDE_DIRS "" CACHE INTERNAL "SPDK Include Directories")
    set(XSAN_SPDK_LIBRARIES "" CACHE INTERNAL "SPDK Libraries")
    set(XSAN_SPDK_LIBRARY_DIRS "" CACHE INTERNAL "SPDK Library Directories")
    set(XSAN_SPDK_CFLAGS "" CACHE INTERNAL "SPDK Compile Flags")
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
