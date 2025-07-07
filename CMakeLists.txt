cmake_minimum_required(VERSION 3.16)
project(xsan VERSION 1.0.0 LANGUAGES C)

# 设置 C 标准
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# 包含目录
include_directories(src/include)

# 查找依赖库
find_package(PkgConfig REQUIRED)

# libvirt
pkg_check_modules(LIBVIRT REQUIRED libvirt)

# LevelDB
find_library(LEVELDB_LIB leveldb REQUIRED)

# JSON-C
pkg_check_modules(JSON_C REQUIRED json-c)

# UUID
find_library(UUID_LIB uuid REQUIRED)

# Protobuf-C
pkg_check_modules(PROTOBUF_C REQUIRED libprotobuf-c)

# 线程库
find_package(Threads REQUIRED)

# 源文件
set(XSAN_SOURCES
    # 工具库
    src/utils/log.c
    src/utils/memory.c
    src/utils/string_utils.c
    src/utils/time_utils.c
    src/utils/config.c
    src/utils/hashtable.c
    src/utils/list.c
    src/utils/ring_buffer.c
    
    # 网络模块
    src/network/socket.c
    src/network/event_loop.c
    src/network/protocol.c
    
    # 集群管理
    src/cluster/node_discovery.c
    src/cluster/heartbeat.c
    src/cluster/node_manager.c
    src/cluster/leader_election.c
    src/cluster/cluster_init.c
    
    # 存储引擎
    src/storage/metadata.c
    src/storage/volume.c
    src/storage/block_index.c
    src/storage/local_storage.c
    src/storage/block_allocator.c
    src/storage/device_manager.c
    src/storage/consistent_hash.c
    src/storage/placement.c
)

# 头文件
set(XSAN_HEADERS
    src/include/xsan.h
    src/include/xsan_types.h
    src/include/xsan_error.h
    src/include/xsan_log.h
    src/include/xsan_memory.h
    src/include/xsan_network.h
    src/include/xsan_cluster.h
    src/include/xsan_storage.h
)

# 创建静态库
add_library(xsan_static STATIC ${XSAN_SOURCES})
target_link_libraries(xsan_static 
    ${LIBVIRT_LIBRARIES}
    ${LEVELDB_LIB}
    ${JSON_C_LIBRARIES}
    ${UUID_LIB}
    ${PROTOBUF_C_LIBRARIES}
    Threads::Threads
    m
)

# 设置库的包含目录
target_include_directories(xsan_static PUBLIC
    ${LIBVIRT_INCLUDE_DIRS}
    ${JSON_C_INCLUDE_DIRS}
    ${PROTOBUF_C_INCLUDE_DIRS}
)

# 主程序 - 节点守护进程
add_executable(xsan-node
    src/main/xsan_node.c
)
target_link_libraries(xsan-node xsan_static)

# 命令行工具
add_executable(xsan-cli
    src/main/xsan_cli.c
)
target_link_libraries(xsan-cli xsan_static)

# 调试工具
add_executable(xsan-debug
    src/main/xsan_debug.c
)
target_link_libraries(xsan-debug xsan_static)

# 安装目标
install(TARGETS xsan-node xsan-cli xsan-debug
    RUNTIME DESTINATION bin
)

install(FILES ${XSAN_HEADERS}
    DESTINATION include/xsan
)

install(TARGETS xsan_static
    ARCHIVE DESTINATION lib
)

# 测试
enable_testing()

# 查找 CUnit
find_library(CUNIT_LIB cunit)
if(CUNIT_LIB)
    # 单元测试
    file(GLOB TEST_SOURCES "tests/unit/*.c")
    
    add_executable(xsan_tests ${TEST_SOURCES})
    target_link_libraries(xsan_tests 
        xsan_static 
        ${CUNIT_LIB}
    )
    
    add_test(NAME unit_tests COMMAND xsan_tests)
endif()

# 内存检查目标 (需要 Valgrind)
find_program(VALGRIND_PATH valgrind)
if(VALGRIND_PATH)
    add_custom_target(memcheck
        COMMAND ${VALGRIND_PATH} 
            --tool=memcheck 
            --leak-check=full 
            --show-leak-kinds=all
            --track-origins=yes
            --verbose
            $<TARGET_FILE:xsan_tests>
        DEPENDS xsan_tests
    )
endif()

# 代码覆盖率 (需要 gcov)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_COMPILER_IS_GNUCC)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
        
        find_program(GCOV_PATH gcov)
        find_program(LCOV_PATH lcov)
        find_program(GENHTML_PATH genhtml)
        
        if(GCOV_PATH AND LCOV_PATH AND GENHTML_PATH)
            add_custom_target(coverage
                COMMAND ${LCOV_PATH} --directory . --capture --output-file coverage.info
                COMMAND ${LCOV_PATH} --remove coverage.info '/usr/*' --output-file coverage.info
                COMMAND ${LCOV_PATH} --list coverage.info
                COMMAND ${GENHTML_PATH} coverage.info --output-directory coverage_html
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            )
        endif()
    endif()
endif()

# 文档生成 (需要 Doxygen)
find_package(Doxygen)
if(DOXYGEN_FOUND)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/docs/Doxyfile.in 
                   ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    
    add_custom_target(docs
        ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen" VERBATIM
    )
endif()

# 代码格式化 (需要 clang-format)
find_program(CLANG_FORMAT_PATH clang-format)
if(CLANG_FORMAT_PATH)
    file(GLOB_RECURSE ALL_SOURCE_FILES 
        ${CMAKE_SOURCE_DIR}/src/*.c
        ${CMAKE_SOURCE_DIR}/src/*.h
        ${CMAKE_SOURCE_DIR}/tests/*.c
    )
    
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_PATH} -i ${ALL_SOURCE_FILES}
        COMMENT "Formatting source code with clang-format"
    )
endif()

# 静态分析 (需要 cppcheck)
find_program(CPPCHECK_PATH cppcheck)
if(CPPCHECK_PATH)
    add_custom_target(analysis
        COMMAND ${CPPCHECK_PATH}
            --enable=all
            --std=c99
            --verbose
            --quiet
            --error-exitcode=1
            ${CMAKE_SOURCE_DIR}/src
        COMMENT "Running static analysis with cppcheck"
    )
endif()

# 打印配置信息
message(STATUS "")
message(STATUS "XSAN Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  C Flags: ${CMAKE_C_FLAGS}")
message(STATUS "")
message(STATUS "Dependencies:")
message(STATUS "  libvirt: ${LIBVIRT_LIBRARIES}")
message(STATUS "  LevelDB: ${LEVELDB_LIB}")
message(STATUS "  JSON-C: ${JSON_C_LIBRARIES}")
message(STATUS "  UUID: ${UUID_LIB}")
message(STATUS "  Protobuf-C: ${PROTOBUF_C_LIBRARIES}")
message(STATUS "")
message(STATUS "Optional tools:")
message(STATUS "  CUnit: ${CUNIT_LIB}")
message(STATUS "  Valgrind: ${VALGRIND_PATH}")
message(STATUS "  Doxygen: ${DOXYGEN_FOUND}")
message(STATUS "  clang-format: ${CLANG_FORMAT_PATH}")
message(STATUS "  cppcheck: ${CPPCHECK_PATH}")
message(STATUS "")
