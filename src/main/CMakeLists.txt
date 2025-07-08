# XSAN Main Programs
# 主程序构建 - 依赖所有模块

# 收集所有模块的库
set(XSAN_ALL_LIBRARIES
    xsan_utils
    xsan_common
    xsan_network
    xsan_storage
    xsan_cluster
    xsan_replication
    xsan_policy
    xsan_virtualization
)

# 主程序 - 节点守护进程
add_executable(xsan-node xsan_node.c)
target_link_libraries(xsan-node ${XSAN_ALL_LIBRARIES})

# 命令行工具
add_executable(xsan-cli xsan_cli.c)
target_link_libraries(xsan-cli ${XSAN_ALL_LIBRARIES})

# 调试工具
add_executable(xsan-debug xsan_debug.c)
target_link_libraries(xsan-debug ${XSAN_ALL_LIBRARIES})

# 设置包含目录
target_include_directories(xsan-node PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/include
)

target_include_directories(xsan-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/include
)

target_include_directories(xsan-debug PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/include
)

# 安装目标
install(TARGETS xsan-node xsan-cli xsan-debug
    RUNTIME DESTINATION bin
)

# 创建 XSAN 总库 (用于外部链接)
add_library(xsan STATIC)
target_link_libraries(xsan ${XSAN_ALL_LIBRARIES})

# 安装开发库
install(TARGETS xsan ${XSAN_ALL_LIBRARIES}
    ARCHIVE DESTINATION lib
)
