# XSAN Utils Module
# 基础工具库 - 不依赖其他模块

set(XSAN_UTILS_SOURCES
    log.c
    memory.c
    string_utils.c
    time_utils.c
    config.c
    hashtable.c
    list.c
    ring_buffer.c
    thread_pool.c
    atomic.c
)

set(XSAN_UTILS_HEADERS
    ../include/xsan_log.h
    ../include/xsan_memory.h
    ../include/xsan_config.h
    ../include/xsan_utils.h
)

# 创建 utils 静态库
add_library(xsan_utils STATIC ${XSAN_UTILS_SOURCES})

# 设置包含目录
target_include_directories(xsan_utils PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
)

# 链接依赖
target_link_libraries(xsan_utils
    ${JSON_C_LIBRARIES}
    Threads::Threads
    m
)

# 设置编译属性
set_target_properties(xsan_utils PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_UTILS_LIBRARIES xsan_utils PARENT_SCOPE)
set(XSAN_UTILS_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
