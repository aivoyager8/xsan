# XSAN Storage Module
# 存储引擎模块 - 依赖 utils, common, network 模块

set(XSAN_STORAGE_SOURCES
    disk_manager.c
    volume_manager.c
    # metadata.c # Keep for now, might be needed for persistence
    # volume.c # Commenting out, assuming volume_manager.c is the current focus
    # block_index.c
    # local_storage.c
    # block_allocator.c
    # device_manager.c # This might be what disk_manager.c replaced or works with
    # consistent_hash.c
    # placement.c
    # cache.c
    # io_engine.c
    # disk_group.c # Replaced by disk_manager.c
)

# Add headers for the new managers if not already covered
set(XSAN_STORAGE_HEADERS
    ../include/xsan_storage.h       # Main storage types (xsan_disk_t, xsan_disk_group_t, xsan_volume_t)
    ../include/xsan_disk_manager.h  # Header for disk_manager
    ../include/xsan_volume_manager.h # Header for volume_manager
    # ../include/xsan_metadata.h    # Keep if metadata.c is active
    # ../include/xsan_volume.h      # Keep if volume.c is active and different from volume_manager
    # ../include/xsan_block.h
    # ../../include/xsan_storage.h # This seems redundant if src/include/xsan_storage.h is the primary
)

# 创建 storage 静态库
add_library(xsan_storage STATIC ${XSAN_STORAGE_SOURCES})

# 设置包含目录
target_include_directories(xsan_storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(xsan_storage PUBLIC # Use PUBLIC to propagate interface includes from dependencies
    xsan_utils      # For logging, memory, string utils
    xsan_common     # For error types, list, hashtable, ringbuffer, (spdk/uuid.h via xsan_types.h)
    xsan_bdev       # disk_manager and volume_manager (via xsan_io) depend on xsan_bdev
    xsan_io         # volume_manager depends on xsan_io for async operations
    xsan_metadata   # For using the RocksDB wrapper
    # xsan_network  # Only if storage directly does network ops, unlikely for disk/vol managers

    ${LEVELDB_LIB}  # This should be removed if we are using RocksDB via xsan_metadata
                    # and if metadata.c (which might have used LevelDB) is no longer active.
                    # For now, keep if other old files in XSAN_STORAGE_SOURCES might use it.
                    # If metadata.c is removed, this should be removed.
    ${UUID_LIB}     # For spdk_uuid_parse etc. if not provided by SPDK's own linking. Usually not needed separately.
                    # SPDK itself should provide UUID functions.
)

if(XSAN_ENABLE_SPDK) # json-c is only used if SPDK and thus metadata persistence is on
    # Link json-c if found by root CMakeLists.txt
    # XSAN_JSON_C_LIBRARIES contains linker flags like -ljson-c or full paths.
    target_link_libraries(xsan_storage PRIVATE ${XSAN_JSON_C_LIBRARIES})
endif()


# 设置编译属性
set_target_properties(xsan_storage PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_STORAGE_LIBRARIES xsan_storage PARENT_SCOPE)
set(XSAN_STORAGE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
