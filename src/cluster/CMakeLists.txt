# XSAN Cluster Module
# 集群管理模块 - 依赖 utils, common, network, storage 模块

set(XSAN_CLUSTER_SOURCES
    node_discovery.c
    heartbeat.c
    node_manager.c
    leader_election.c
    cluster_init.c
    membership.c
    failover.c
    gossip.c
)

set(XSAN_CLUSTER_HEADERS
    ../include/xsan_cluster.h
    ../include/xsan_node.h
    ../include/xsan_membership.h
    ../../include/xsan_cluster.h
)

# 创建 cluster 静态库
add_library(xsan_cluster STATIC ${XSAN_CLUSTER_SOURCES})

# 设置包含目录
target_include_directories(xsan_cluster PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(xsan_cluster
    xsan_utils
    xsan_common
    xsan_network
    xsan_storage
    Threads::Threads
)

# 设置编译属性
set_target_properties(xsan_cluster PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_CLUSTER_LIBRARIES xsan_cluster PARENT_SCOPE)
set(XSAN_CLUSTER_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
