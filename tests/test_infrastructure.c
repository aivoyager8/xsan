/**
 * XSAN 基础设施模块测试
 * 
 * 测试内存管理、字符串工具和配置解析功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "xsan_memory.h"
#include "xsan_string_utils.h"
#include "xsan_config.h"
#include "xsan_log.h"
#include "xsan_error.h"

/**
 * 测试内存管理功能
 */
static void test_memory_management(void)
{
    printf("测试内存管理功能...\n");
    
    /* 初始化内存管理器 */
    xsan_error_t ret = xsan_memory_init(true);
    assert(ret == XSAN_OK);
    
    /* 测试基本的内存分配和释放 */
    void *ptr1 = xsan_malloc(1024);
    assert(ptr1 != NULL);
    
    void *ptr2 = xsan_calloc(10, 100);
    assert(ptr2 != NULL);
    
    char *str = xsan_strdup("Hello, XSAN!");
    assert(str != NULL);
    assert(strcmp(str, "Hello, XSAN!") == 0);
    
    /* 测试内存池 */
    xsan_memory_pool_config_t pool_config = {
        .block_size = 64,
        .initial_blocks = 10,
        .max_blocks = 100,
        .thread_safe = true
    };
    
    xsan_memory_pool_t *pool = xsan_memory_pool_create(&pool_config);
    assert(pool != NULL);
    
    void *pool_ptr = xsan_memory_pool_alloc(pool);
    assert(pool_ptr != NULL);
    
    xsan_memory_pool_free(pool, pool_ptr);
    xsan_memory_pool_destroy(pool);
    
    /* 打印内存统计信息 */
    xsan_memory_print_stats();
    
    /* 释放内存 */
    xsan_free(ptr1);
    xsan_free(ptr2);
    xsan_free(str);
    
    /* 检查内存泄漏 */
    bool has_leaks = xsan_memory_check_leaks();
    assert(!has_leaks);
    
    xsan_memory_cleanup();
    printf("✓ 内存管理功能测试通过\n\n");
}

/**
 * 测试字符串工具功能
 */
static void test_string_utils(void)
{
    printf("测试字符串工具功能...\n");
    
    /* 测试字符串分割 */
    char *tokens[10];
    size_t token_count = xsan_strsplit("a,b,c,d", ",", tokens, 10);
    assert(token_count == 4);
    assert(strcmp(tokens[0], "a") == 0);
    assert(strcmp(tokens[1], "b") == 0);
    assert(strcmp(tokens[2], "c") == 0);
    assert(strcmp(tokens[3], "d") == 0);
    
    /* 释放分割后的字符串 */
    for (size_t i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    
    /* 测试字符串修剪 */
    char *trimmed = xsan_strtrim("  hello world  ");
    assert(strcmp(trimmed, "hello world") == 0);
    free(trimmed);
    
    /* 测试字符串大小写转换 */
    char *upper = xsan_strupper("hello");
    assert(strcmp(upper, "HELLO") == 0);
    free(upper);
    
    char *lower = xsan_strlower("WORLD");
    assert(strcmp(lower, "world") == 0);
    free(lower);
    
    /* 测试字符串前后缀检查 */
    assert(xsan_str_starts_with("hello world", "hello") == true);
    assert(xsan_str_ends_with("hello world", "world") == true);
    assert(xsan_str_contains("hello world", "lo wo") == true);
    
    /* 测试字符串替换 */
    char *replaced = xsan_str_replace("hello world", "world", "XSAN");
    assert(strcmp(replaced, "hello XSAN") == 0);
    free(replaced);
    
    /* 测试类型转换 */
    int int_val;
    assert(xsan_str_to_int("123", &int_val) == true);
    assert(int_val == 123);
    
    bool bool_val;
    assert(xsan_str_to_bool("true", &bool_val) == true);
    assert(bool_val == true);
    
    /* 测试字节数转换 */
    char buffer[64];
    xsan_bytes_to_human_readable(1024*1024, buffer, sizeof(buffer));
    printf("1MB = %s\n", buffer);
    
    printf("✓ 字符串工具功能测试通过\n\n");
}

/**
 * 测试配置管理功能
 */
static void test_config_management(void)
{
    printf("测试配置管理功能...\n");
    
    /* 创建配置管理器 */
    xsan_config_t *config = xsan_config_create();
    assert(config != NULL);
    
    /* 测试配置项设置和获取 */
    xsan_config_set_string(config, "node.name", "test-node");
    xsan_config_set_int(config, "node.port", 8080);
    xsan_config_set_bool(config, "node.enable_ssl", true);
    
    assert(strcmp(xsan_config_get_string(config, "node.name", ""), "test-node") == 0);
    assert(xsan_config_get_int(config, "node.port", 0) == 8080);
    assert(xsan_config_get_bool(config, "node.enable_ssl", false) == true);
    
    /* 测试配置项存在性检查 */
    assert(xsan_config_has_key(config, "node.name") == true);
    assert(xsan_config_has_key(config, "non.existent") == false);
    
    /* 测试从字符串加载配置 */
    const char *config_str = 
        "# Test configuration\n"
        "node.id = node-001\n"
        "node.port = 9090\n"
        "storage.block_size = 4096\n"
        "cluster.enable_auto_failover = false\n";
    
    xsan_config_load_from_string(config, config_str);
    
    assert(strcmp(xsan_config_get_string(config, "node.id", ""), "node-001") == 0);
    assert(xsan_config_get_int(config, "node.port", 0) == 9090);
    assert(xsan_config_get_int(config, "storage.block_size", 0) == 4096);
    assert(xsan_config_get_bool(config, "cluster.enable_auto_failover", true) == false);
    
    /* 测试结构化配置加载 */
    xsan_node_config_t node_config;
    xsan_config_load_node_config(config, &node_config);
    
    assert(strcmp(node_config.node_id, "node-001") == 0);
    assert(node_config.port == 9090);
    
    /* 打印配置信息 */
    xsan_config_print(config);
    
    /* 清理 */
    xsan_config_destroy(config);
    
    printf("✓ 配置管理功能测试通过\n\n");
}

/**
 * 测试日志功能
 */
static void test_logging(void)
{
    printf("测试日志功能...\n");
    
    /* 初始化日志系统 */
    xsan_log_config_t log_config = {
        .log_file = "",
        .level = XSAN_LOG_LEVEL_DEBUG,
        .console_output = true,
        .file_output = false,
        .max_file_size = 10 * 1024 * 1024,
        .max_file_count = 5
    };
    
    xsan_error_t ret = xsan_log_init(&log_config);
    assert(ret == XSAN_OK);
    
    /* 测试不同级别的日志 */
    XSAN_LOG_DEBUG("这是一条调试日志");
    XSAN_LOG_INFO("这是一条信息日志");
    XSAN_LOG_WARN("这是一条警告日志");
    XSAN_LOG_ERROR("这是一条错误日志");
    
    /* 测试带参数的日志 */
    XSAN_LOG_INFO("节点 %s 在端口 %d 上启动", "test-node", 8080);
    
    xsan_log_cleanup();
    printf("✓ 日志功能测试通过\n\n");
}

/**
 * 主函数
 */
int main(int argc, char *argv[])
{
    printf("=== XSAN 基础设施模块测试 ===\n\n");
    
    /* 测试各个模块 */
    test_logging();
    test_memory_management();
    test_string_utils();
    test_config_management();
    
    printf("=== 所有测试通过! ===\n");
    
    return 0;
}
