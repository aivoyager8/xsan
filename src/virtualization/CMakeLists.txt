# XSAN Virtualization Module
# 虚拟化集成模块 - 依赖 utils, common, storage, policy 模块

set(XSAN_VIRTUALIZATION_SOURCES
    libvirt_integration.c
    vm_storage.c
    disk_provisioning.c
    storage_pool.c
    snapshot_manager.c
)

set(XSAN_VIRTUALIZATION_HEADERS
    ../include/xsan_virtualization.h
    ../include/xsan_libvirt.h
    ../../include/xsan_virtualization.h
)

# 创建 virtualization 静态库
add_library(xsan_virtualization STATIC ${XSAN_VIRTUALIZATION_SOURCES})

# 设置包含目录
target_include_directories(xsan_virtualization PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
    ${LIBVIRT_INCLUDE_DIRS}
)

# 链接依赖
target_link_libraries(xsan_virtualization
    xsan_utils
    xsan_common
    xsan_storage
    xsan_policy
    ${LIBVIRT_LIBRARIES}
)

# 设置编译属性
set_target_properties(xsan_virtualization PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_VIRTUALIZATION_LIBRARIES xsan_virtualization PARENT_SCOPE)
set(XSAN_VIRTUALIZATION_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
