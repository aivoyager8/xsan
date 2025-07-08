# XSAN Storage Module
# 存储引擎模块 - 依赖 utils, common, network 模块

set(XSAN_STORAGE_SOURCES
    metadata.c
    volume.c
    block_index.c
    local_storage.c
    block_allocator.c
    device_manager.c
    consistent_hash.c
    placement.c
    cache.c
    io_engine.c
    disk_group.c
)

set(XSAN_STORAGE_HEADERS
    ../include/xsan_storage.h
    ../include/xsan_metadata.h
    ../include/xsan_volume.h
    ../include/xsan_block.h
    ../../include/xsan_storage.h
)

# 创建 storage 静态库
add_library(xsan_storage STATIC ${XSAN_STORAGE_SOURCES})

# 设置包含目录
target_include_directories(xsan_storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(xsan_storage
    xsan_utils
    xsan_common
    xsan_network
    ${LEVELDB_LIB}
    ${UUID_LIB}
)

# 设置编译属性
set_target_properties(xsan_storage PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_STORAGE_LIBRARIES xsan_storage PARENT_SCOPE)
set(XSAN_STORAGE_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
