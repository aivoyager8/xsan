# XSAN Policy Module
# 存储策略模块 - 依赖 utils, common, storage 模块

set(XSAN_POLICY_SOURCES
    policy_engine.c
    storage_policy.c
    qos_policy.c
    placement_policy.c
    tiering_policy.c
)

set(XSAN_POLICY_HEADERS
    ../include/xsan_policy.h
    ../include/xsan_qos.h
    ../../include/xsan_policy.h
)

# 创建 policy 静态库
add_library(xsan_policy STATIC ${XSAN_POLICY_SOURCES})

# 设置包含目录
target_include_directories(xsan_policy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(xsan_policy
    xsan_utils
    xsan_common
    xsan_storage
)

# 设置编译属性
set_target_properties(xsan_policy PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_POLICY_LIBRARIES xsan_policy PARENT_SCOPE)
set(XSAN_POLICY_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
