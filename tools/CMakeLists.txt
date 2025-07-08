# XSAN Tools
# 命令行工具和实用程序

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

# 集群初始化工具
add_executable(xsan-cluster-init cluster_init.c)
target_link_libraries(xsan-cluster-init ${XSAN_ALL_LIBRARIES})

# 存储设备管理工具
add_executable(xsan-device-manager device_manager.c)
target_link_libraries(xsan-device-manager ${XSAN_ALL_LIBRARIES})

# 数据恢复工具
add_executable(xsan-recovery-tool recovery_tool.c)
target_link_libraries(xsan-recovery-tool ${XSAN_ALL_LIBRARIES})

# 性能监控工具
add_executable(xsan-monitor monitor.c)
target_link_libraries(xsan-monitor ${XSAN_ALL_LIBRARIES})

# 配置验证工具
add_executable(xsan-config-validator config_validator.c)
target_link_libraries(xsan-config-validator ${XSAN_ALL_LIBRARIES})

# 设置包含目录
foreach(tool xsan-cluster-init xsan-device-manager xsan-recovery-tool xsan-monitor xsan-config-validator)
    target_include_directories(${tool} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/include
    )
endforeach()

# 安装工具
install(TARGETS 
    xsan-cluster-init 
    xsan-device-manager 
    xsan-recovery-tool 
    xsan-monitor 
    xsan-config-validator
    RUNTIME DESTINATION bin
)
