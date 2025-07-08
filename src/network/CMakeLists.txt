# XSAN Network Module
# 网络通信模块 - 依赖 utils 和 common 模块

set(XSAN_NETWORK_SOURCES
    socket.c
    event_loop.c
    protocol.c
    message.c
    rpc.c
    connection.c
    server.c
    client.c
)

set(XSAN_NETWORK_HEADERS
    ../include/xsan_network.h
    ../include/xsan_protocol.h
    ../include/xsan_rpc.h
)

# 创建 network 静态库
add_library(xsan_network STATIC ${XSAN_NETWORK_SOURCES})

# 设置包含目录
target_include_directories(xsan_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include
    ${PROTOBUF_C_INCLUDE_DIRS}
)

# 链接依赖
target_link_libraries(xsan_network
    xsan_utils
    xsan_common
    ${PROTOBUF_C_LIBRARIES}
    Threads::Threads
)

# 设置编译属性
set_target_properties(xsan_network PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# 导出目标
set(XSAN_NETWORK_LIBRARIES xsan_network PARENT_SCOPE)
set(XSAN_NETWORK_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/../include PARENT_SCOPE)
