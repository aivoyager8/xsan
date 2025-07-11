# XSAN Common Module
# 公共数据结构和类型定义 - 依赖 utils 模块

set(XSAN_COMMON_SOURCES
    error.c
    version.c
    hashtable.c
    list.c
    ring_buffer.c
)

set(XSAN_COMMON_HEADERS
    ../include/xsan_error.h
    ../include/xsan_types.h
    ../../include/xsan_common.h
)

# 创建 common 静态库
add_library(xsan_common STATIC ${XSAN_COMMON_SOURCES})

# 设置包含目录
target_include_directories(xsan_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(xsan_common
    xsan_utils
)

# 设置编译属性
set_target_properties(xsan_common PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_COMMON_LIBRARIES xsan_common PARENT_SCOPE)
set(XSAN_COMMON_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
