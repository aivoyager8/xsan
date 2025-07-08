# XSAN Tests
# 测试构建 - 依赖所有模块

enable_testing()

# 查找 CUnit
find_library(CUNIT_LIB cunit)
if(NOT CUNIT_LIB)
    message(WARNING "CUnit not found, tests will not be built")
    return()
endif()

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

# 单元测试源文件
set(UNIT_TEST_SOURCES
    unit/test_main.c
    unit/test_utils.c
    unit/test_common.c
    unit/test_network.c
    unit/test_storage.c
    unit/test_cluster.c
    unit/test_replication.c
    unit/test_policy.c
    unit/test_virtualization.c
)

# 集成测试源文件
set(INTEGRATION_TEST_SOURCES
    integration/test_integration_main.c
    integration/test_cluster_integration.c
    integration/test_storage_integration.c
    integration/test_replication_integration.c
)

# 单元测试可执行文件
add_executable(xsan_unit_tests ${UNIT_TEST_SOURCES})
target_link_libraries(xsan_unit_tests 
    ${XSAN_ALL_LIBRARIES}
    ${CUNIT_LIB}
)

target_include_directories(xsan_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/include
)

# 集成测试可执行文件
add_executable(xsan_integration_tests ${INTEGRATION_TEST_SOURCES})
target_link_libraries(xsan_integration_tests 
    ${XSAN_ALL_LIBRARIES}
    ${CUNIT_LIB}
)

target_include_directories(xsan_integration_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/include
)

# 添加测试
add_test(NAME unit_tests COMMAND xsan_unit_tests)
add_test(NAME integration_tests COMMAND xsan_integration_tests)

# 设置测试环境变量
set_tests_properties(unit_tests PROPERTIES
    ENVIRONMENT "XSAN_TEST_MODE=1"
)

set_tests_properties(integration_tests PROPERTIES
    ENVIRONMENT "XSAN_TEST_MODE=1"
)

# 性能测试
add_executable(xsan_perf_tests 
    perf/test_perf_main.c
    perf/test_storage_perf.c
    perf/test_network_perf.c
)

target_link_libraries(xsan_perf_tests ${XSAN_ALL_LIBRARIES})

target_include_directories(xsan_perf_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/include
)

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
            $<TARGET_FILE:xsan_unit_tests>
        DEPENDS xsan_unit_tests
    )
endif()

# 代码覆盖率 (需要 gcov)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_COMPILER_IS_GNUCC)
        target_compile_options(xsan_unit_tests PRIVATE --coverage)
        target_link_options(xsan_unit_tests PRIVATE --coverage)
        
        target_compile_options(xsan_integration_tests PRIVATE --coverage)
        target_link_options(xsan_integration_tests PRIVATE --coverage)
        
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
