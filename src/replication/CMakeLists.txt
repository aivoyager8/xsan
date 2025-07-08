# XSAN Replication Module
# 数据复制模块 - 依赖 utils, common, network, storage, cluster 模块

set(XSAN_REPLICATION_SOURCES
    replica_manager.c
    replica_set.c
    data_sync.c
    consistency.c
    recovery.c
    snapshot.c
    log_replication.c
)

set(XSAN_REPLICATION_HEADERS
    ../include/xsan_replication.h
    ../include/xsan_replica.h
    ../include/xsan_consistency.h
    ../../include/xsan_replication.h
)

# 创建 replication 静态库
add_library(xsan_replication STATIC ${XSAN_REPLICATION_SOURCES})

# 设置包含目录
target_include_directories(xsan_replication PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(xsan_replication
    xsan_utils
    xsan_common
    xsan_network
    xsan_storage
    xsan_cluster
    Threads::Threads
)

# 设置编译属性
set_target_properties(xsan_replication PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_REPLICATION_LIBRARIES xsan_replication PARENT_SCOPE)
set(XSAN_REPLICATION_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
