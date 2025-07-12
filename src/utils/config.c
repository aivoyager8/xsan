#include <spdk/uuid.h>
/**
 * XSAN 配置管理模块实现
 * 
 * 提供配置文件解析和配置参数管理功能
 */

#include "xsan_memory.h"
#include "xsan_config.h"
#include <spdk/uuid.h>
#include "xsan_memory.h"
#include "xsan_string_utils.h"
#include "xsan_log.h"
#include "xsan_error.h"
#include "xsan_types.h" // For xsan_node_t, INET_ADDRSTRLEN, XSAN_MAX_NAME_LEN
#include "spdk/uuid.h"  // For spdk_uuid_parse, spdk_uuid_get_string
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <spdk/uuid.h>

/* 默认配置值 */
#define DEFAULT_PORT                    8080
#define DEFAULT_MAX_CONNECTIONS         1000
#define DEFAULT_HEARTBEAT_INTERVAL      30
#define DEFAULT_ELECTION_TIMEOUT        150
#define DEFAULT_BLOCK_SIZE              (4 * 1024)
#define DEFAULT_MAX_FILE_SIZE           (1024 * 1024 * 1024)
#define DEFAULT_CACHE_SIZE              (100 * 1024 * 1024)
#define DEFAULT_IO_THREADS              4
#define DEFAULT_SYNC_INTERVAL           60
#define DEFAULT_REPLICATION_FACTOR      3
#define DEFAULT_MIN_NODES               3
#define DEFAULT_MAX_NODES               64
#define DEFAULT_QUORUM_SIZE             2
#define DEFAULT_NETWORK_TIMEOUT         30
#define DEFAULT_RECONNECT_INTERVAL      5
#define DEFAULT_FAILOVER_TIMEOUT        300

/**
 * 查找配置项
 */
static xsan_config_item_t *find_config_item(xsan_config_t *config, const char *key)
{
    if (!config || !key) {
        return NULL;
    }
    
    xsan_config_item_t *item = config->items;
    while (item) {
        if (strcmp(item->key, key) == 0) {
            return item;
        }
        item = item->next;
    }
    
    return NULL;
}

/**
 * 释放配置项
 */
static void free_config_item(xsan_config_item_t *item)
{
    if (!item) {
        return;
    }
    
    xsan_free(item->key);
    if (item->type == XSAN_CONFIG_TYPE_STRING) {
        xsan_free(item->value.string_val);
    }
    xsan_free(item);
}

/**
 * 添加或更新配置项
 */
static bool set_config_item(xsan_config_t *config, const char *key, 
                           xsan_config_type_t type, const void *value)
{
    if (!config || !key || !value) {
        return false;
    }
    
    xsan_config_item_t *item = find_config_item(config, key);
    if (item) {
        /* 更新现有项 */
        if (item->type == XSAN_CONFIG_TYPE_STRING) {
            xsan_free(item->value.string_val);
        }
        item->type = type;
    } else {
        /* 创建新项 */
        item = xsan_malloc(sizeof(xsan_config_item_t));
        if (!item) {
            return false;
        }
        
        item->key = xsan_strdup(key);
        if (!item->key) {
            xsan_free(item);
            return false;
        }
        
        item->type = type;
        item->next = config->items;
        config->items = item;
        config->item_count++;
    }
    
    /* 设置值 */
    switch (type) {
        case XSAN_CONFIG_TYPE_STRING:
            item->value.string_val = xsan_strdup((const char*)value);
            if (!item->value.string_val) {
                return false;
            }
            break;
        case XSAN_CONFIG_TYPE_INT:
            item->value.int_val = *(const int*)value;
            break;
        case XSAN_CONFIG_TYPE_LONG:
            item->value.long_val = *(const long*)value;
            break;
        case XSAN_CONFIG_TYPE_DOUBLE:
            item->value.double_val = *(const double*)value;
            break;
        case XSAN_CONFIG_TYPE_BOOL:
            item->value.bool_val = *(const bool*)value;
            break;
    }
    
    return true;
}

/**
 * 创建配置管理器
 */
xsan_config_t *xsan_config_create(void)
{
    xsan_config_t *config = xsan_malloc(sizeof(xsan_config_t));
    if (!config) {
        return NULL;
    }
    
    config->items = NULL;
    config->item_count = 0;
    config->config_file = NULL;
    config->auto_reload = false;
    config->last_modified = 0;
    
    return config;
}

/**
 * 销毁配置管理器
 */
void xsan_config_destroy(xsan_config_t *config)
{
    if (!config) {
        return;
    }
    
    xsan_config_clear(config);
    xsan_free(config->config_file);
    xsan_free(config);
}

/**
 * 从文件加载配置
 */
bool xsan_config_load_from_file(xsan_config_t *config, const char *config_file)
{
    if (!config || !config_file) {
        return false;
    }
    
    FILE *file = fopen(config_file, "r");
    if (!file) {
        XSAN_LOG_ERROR("Failed to open config file: %s (%s)", config_file, strerror(errno));
        return false;
    }
    
    /* 获取文件修改时间 */
    struct stat st;
    if (stat(config_file, &st) == 0) {
        config->last_modified = st.st_mtime;
    }
    
    /* 设置配置文件路径 */
    xsan_free(config->config_file);
    config->config_file = xsan_strdup(config_file);
    
    /* 清空现有配置 */
    xsan_config_clear(config);
    
    char line[1024];
    int line_number = 0;
    bool success = true;
    
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        
        /* 解析配置行 */
        char key[256], value[512];
        if (xsan_parse_config_line(line, key, value, sizeof(key), sizeof(value))) {
            /* 尝试解析为不同类型 */
            int int_val;
            long long_val;
            double double_val;
            bool bool_val;
            
            if (xsan_str_to_int(value, &int_val)) {
                xsan_config_set_int(config, key, int_val);
            } else if (xsan_str_to_long(value, &long_val)) {
                xsan_config_set_long(config, key, long_val);
            } else if (xsan_str_to_double(value, &double_val)) {
                xsan_config_set_double(config, key, double_val);
            } else if (xsan_str_to_bool(value, &bool_val)) {
                xsan_config_set_bool(config, key, bool_val);
            } else {
                xsan_config_set_string(config, key, value);
            }
        }
    }
    
    fclose(file);
    
    if (success) {
        XSAN_LOG_INFO("Loaded %zu configuration items from %s", 
                     config->item_count, config_file);
    }
    
    return success;
}

/**
 * 从字符串加载配置
 */
bool xsan_config_load_from_string(xsan_config_t *config, const char *config_str)
{
    if (!config || !config_str) {
        return false;
    }
    
    /* 清空现有配置 */
    xsan_config_clear(config);
    
    char *str_copy = xsan_strdup(config_str);
    if (!str_copy) {
        return false;
    }
    
    char *line = strtok(str_copy, "\n");
    while (line) {
        char key[256], value[512];
        if (xsan_parse_config_line(line, key, value, sizeof(key), sizeof(value))) {
            /* 尝试解析为不同类型 */
            int int_val;
            long long_val;
            double double_val;
            bool bool_val;
            
            if (xsan_str_to_int(value, &int_val)) {
                xsan_config_set_int(config, key, int_val);
            } else if (xsan_str_to_long(value, &long_val)) {
                xsan_config_set_long(config, key, long_val);
            } else if (xsan_str_to_double(value, &double_val)) {
                xsan_config_set_double(config, key, double_val);
            } else if (xsan_str_to_bool(value, &bool_val)) {
                xsan_config_set_bool(config, key, bool_val);
            } else {
                xsan_config_set_string(config, key, value);
            }
        }
        line = strtok(NULL, "\n");
    }
    
    xsan_free(str_copy);
    
    XSAN_LOG_INFO("Loaded %zu configuration items from string", config->item_count);
    return true;
}

/**
 * 保存配置到文件
 */
bool xsan_config_save_to_file(xsan_config_t *config, const char *config_file)
{
    if (!config || !config_file) {
        return false;
    }
    
    FILE *file = fopen(config_file, "w");
    if (!file) {
        XSAN_LOG_ERROR("Failed to open config file for writing: %s (%s)", 
                      config_file, strerror(errno));
        return false;
    }
    
    fprintf(file, "# XSAN Configuration File\n");
    fprintf(file, "# Generated at %s\n", ctime(&(time_t){time(NULL)}));
    fprintf(file, "\n");
    
    xsan_config_item_t *item = config->items;
    while (item) {
        switch (item->type) {
            case XSAN_CONFIG_TYPE_STRING:
                fprintf(file, "%s = %s\n", item->key, item->value.string_val);
                break;
            case XSAN_CONFIG_TYPE_INT:
                fprintf(file, "%s = %d\n", item->key, item->value.int_val);
                break;
            case XSAN_CONFIG_TYPE_LONG:
                fprintf(file, "%s = %ld\n", item->key, item->value.long_val);
                break;
            case XSAN_CONFIG_TYPE_DOUBLE:
                fprintf(file, "%s = %f\n", item->key, item->value.double_val);
                break;
            case XSAN_CONFIG_TYPE_BOOL:
                fprintf(file, "%s = %s\n", item->key, item->value.bool_val ? "true" : "false");
                break;
        }
        item = item->next;
    }
    
    fclose(file);
    
    XSAN_LOG_INFO("Saved %zu configuration items to %s", config->item_count, config_file);
    return true;
}

/**
 * 设置字符串配置项
 */
bool xsan_config_set_string(xsan_config_t *config, const char *key, const char *value)
{
    return set_config_item(config, key, XSAN_CONFIG_TYPE_STRING, value);
}

/**
 * 设置整数配置项
 */
bool xsan_config_set_int(xsan_config_t *config, const char *key, int value)
{
    return set_config_item(config, key, XSAN_CONFIG_TYPE_INT, &value);
}

/**
 * 设置长整数配置项
 */
bool xsan_config_set_long(xsan_config_t *config, const char *key, long value)
{
    return set_config_item(config, key, XSAN_CONFIG_TYPE_LONG, &value);
}

/**
 * 设置双精度浮点数配置项
 */
bool xsan_config_set_double(xsan_config_t *config, const char *key, double value)
{
    return set_config_item(config, key, XSAN_CONFIG_TYPE_DOUBLE, &value);
}

/**
 * 设置布尔配置项
 */
bool xsan_config_set_bool(xsan_config_t *config, const char *key, bool value)
{
    return set_config_item(config, key, XSAN_CONFIG_TYPE_BOOL, &value);
}

/**
 * 获取字符串配置项
 */
const char *xsan_config_get_string(xsan_config_t *config, const char *key, const char *default_value)
{
    xsan_config_item_t *item = find_config_item(config, key);
    if (item && item->type == XSAN_CONFIG_TYPE_STRING) {
        return item->value.string_val;
    }
    return default_value;
}

/**
 * 获取整数配置项
 */
int xsan_config_get_int(xsan_config_t *config, const char *key, int default_value)
{
    xsan_config_item_t *item = find_config_item(config, key);
    if (item && item->type == XSAN_CONFIG_TYPE_INT) {
        return item->value.int_val;
    }
    return default_value;
}

/**
 * 获取长整数配置项
 */
long xsan_config_get_long(xsan_config_t *config, const char *key, long default_value)
{
    xsan_config_item_t *item = find_config_item(config, key);
    if (item && item->type == XSAN_CONFIG_TYPE_LONG) {
        return item->value.long_val;
    }
    return default_value;
}

/**
 * 获取双精度浮点数配置项
 */
double xsan_config_get_double(xsan_config_t *config, const char *key, double default_value)
{
    xsan_config_item_t *item = find_config_item(config, key);
    if (item && item->type == XSAN_CONFIG_TYPE_DOUBLE) {
        return item->value.double_val;
    }
    return default_value;
}

/**
 * 获取布尔配置项
 */
bool xsan_config_get_bool(xsan_config_t *config, const char *key, bool default_value)
{
    xsan_config_item_t *item = find_config_item(config, key);
    if (item && item->type == XSAN_CONFIG_TYPE_BOOL) {
        return item->value.bool_val;
    }
    return default_value;
}

/**
 * 检查配置项是否存在
 */
bool xsan_config_has_key(xsan_config_t *config, const char *key)
{
    return find_config_item(config, key) != NULL;
}

/**
 * 删除配置项
 */
bool xsan_config_remove_key(xsan_config_t *config, const char *key)
{
    if (!config || !key) {
        return false;
    }
    
    xsan_config_item_t *prev = NULL;
    xsan_config_item_t *item = config->items;
    
    while (item) {
        if (strcmp(item->key, key) == 0) {
            if (prev) {
                prev->next = item->next;
            } else {
                config->items = item->next;
            }
            free_config_item(item);
            config->item_count--;
            return true;
        }
        prev = item;
        item = item->next;
    }
    
    return false;
}

/**
 * 清空所有配置项
 */
void xsan_config_clear(xsan_config_t *config)
{
    if (!config) {
        return;
    }
    
    xsan_config_item_t *item = config->items;
    while (item) {
        xsan_config_item_t *next = item->next;
        free_config_item(item);
        item = next;
    }
    
    config->items = NULL;
    config->item_count = 0;
}

/**
 * 获取配置项数量
 */
size_t xsan_config_get_item_count(xsan_config_t *config)
{
    return config ? config->item_count : 0;
}

/**
 * 检查配置文件是否已修改
 */
bool xsan_config_is_modified(xsan_config_t *config)
{
    if (!config || !config->config_file) {
        return false;
    }
    
    struct stat st;
    if (stat(config->config_file, &st) != 0) {
        return false;
    }
    
    return st.st_mtime > config->last_modified;
}

/**
 * 重载配置文件
 */
bool xsan_config_reload(xsan_config_t *config)
{
    if (!config || !config->config_file) {
        return false;
    }
    
    return xsan_config_load_from_file(config, config->config_file);
}

/**
 * 设置自动重载
 */
void xsan_config_set_auto_reload(xsan_config_t *config, bool auto_reload)
{
    if (config) {
        config->auto_reload = auto_reload;
    }
}

/**
 * 从配置管理器加载节点配置
 */
bool xsan_config_load_node_config(xsan_config_t *config, xsan_node_config_t *node_config)
{
    if (!config || !node_config) {
        return false;
    }
    
    /* 设置默认值 */
    memset(node_config, 0, sizeof(xsan_node_config_t));
    
    /* 加载配置 */
    xsan_strcpy_safe(node_config->node_id, 
                     xsan_config_get_string(config, "node.id", ""), 
                     sizeof(node_config->node_id));
    
    xsan_strcpy_safe(node_config->node_name, 
                     xsan_config_get_string(config, "node.name", ""), 
                     sizeof(node_config->node_name));
    
    xsan_strcpy_safe(node_config->bind_address, 
                     xsan_config_get_string(config, "node.bind_address", "0.0.0.0"), 
                     sizeof(node_config->bind_address));
    
    node_config->port = xsan_config_get_int(config, "node.port", DEFAULT_PORT);
    
    xsan_strcpy_safe(node_config->data_dir, 
                     xsan_config_get_string(config, "node.data_dir", "/var/lib/xsan"), 
                     sizeof(node_config->data_dir));
    
    xsan_strcpy_safe(node_config->log_file, 
                     xsan_config_get_string(config, "node.log_file", "/var/log/xsan/xsan.log"), 
                     sizeof(node_config->log_file));
    
    xsan_strcpy_safe(node_config->log_level, 
                     xsan_config_get_string(config, "node.log_level", "INFO"), 
                     sizeof(node_config->log_level));
    
    node_config->max_connections = xsan_config_get_int(config, "node.max_connections", DEFAULT_MAX_CONNECTIONS);
    node_config->heartbeat_interval = xsan_config_get_int(config, "node.heartbeat_interval", DEFAULT_HEARTBEAT_INTERVAL);
    node_config->election_timeout = xsan_config_get_int(config, "node.election_timeout", DEFAULT_ELECTION_TIMEOUT);
    node_config->enable_ssl = xsan_config_get_bool(config, "node.enable_ssl", false);
    
    xsan_strcpy_safe(node_config->ssl_cert_file, 
                     xsan_config_get_string(config, "node.ssl_cert_file", ""), 
                     sizeof(node_config->ssl_cert_file));
    
    xsan_strcpy_safe(node_config->ssl_key_file, 
                     xsan_config_get_string(config, "node.ssl_key_file", ""), 
                     sizeof(node_config->ssl_key_file));
    
    // Load NVMe-oF target specific config (optional)
    xsan_strcpy_safe(node_config->nvmf_target_nqn,
                     xsan_config_get_string(config, "nvmf.target_nqn", ""), // Default to empty, nvmf_target_init will use default
                     sizeof(node_config->nvmf_target_nqn));

    xsan_strcpy_safe(node_config->nvmf_listen_port,
                     xsan_config_get_string(config, "nvmf.listen_port", "4420"), // Default NVMe-oF TCP port
                     sizeof(node_config->nvmf_listen_port));

    return true;
}

/**
 * 从配置管理器加载存储配置
 */
bool xsan_config_load_storage_config(xsan_config_t *config, xsan_storage_config_t *storage_config)
{
    if (!config || !storage_config) {
        return false;
    }
    
    /* 设置默认值 */
    memset(storage_config, 0, sizeof(xsan_storage_config_t));
    
    /* 加载配置 */
    xsan_strcpy_safe(storage_config->storage_dir, 
                     xsan_config_get_string(config, "storage.dir", "/var/lib/xsan/storage"), 
                     sizeof(storage_config->storage_dir));
    
    storage_config->block_size = xsan_config_get_int(config, "storage.block_size", DEFAULT_BLOCK_SIZE);
    storage_config->max_file_size = xsan_config_get_long(config, "storage.max_file_size", DEFAULT_MAX_FILE_SIZE);
    storage_config->cache_size = xsan_config_get_long(config, "storage.cache_size", DEFAULT_CACHE_SIZE);
    storage_config->io_threads = xsan_config_get_int(config, "storage.io_threads", DEFAULT_IO_THREADS);
    storage_config->sync_interval = xsan_config_get_int(config, "storage.sync_interval", DEFAULT_SYNC_INTERVAL);
    storage_config->enable_compression = xsan_config_get_bool(config, "storage.enable_compression", false);
    
    xsan_strcpy_safe(storage_config->compression_algorithm, 
                     xsan_config_get_string(config, "storage.compression_algorithm", "lz4"), 
                     sizeof(storage_config->compression_algorithm));
    
    storage_config->replication_factor = xsan_config_get_int(config, "storage.replication_factor", DEFAULT_REPLICATION_FACTOR);
    storage_config->enable_checksums = xsan_config_get_bool(config, "storage.enable_checksums", true);
    
    return true;
}

/**
 * 从配置管理器加载集群配置
 */
bool xsan_config_load_cluster_config(xsan_config_t *config, xsan_cluster_config_t *cluster_config)
{
    if (!config || !cluster_config) {
        return false;
    }
    
    /* 设置默认值 */
    memset(cluster_config, 0, sizeof(xsan_cluster_config_t));
    
    /* 加载配置 */
    xsan_strcpy_safe(cluster_config->cluster_name, 
                     xsan_config_get_string(config, "cluster.name", "xsan-cluster"), 
                     sizeof(cluster_config->cluster_name));
    
    /* 解析种子节点列表 */
    const char *seed_nodes_str = xsan_config_get_string(config, "cluster.seed_nodes", "");
    cluster_config->seed_node_count = 0; // Initialize count

    if (strlen(seed_nodes_str) > 0) {
        char *str_copy = xsan_strdup(seed_nodes_str);
        if (!str_copy) {
            XSAN_LOG_ERROR("Failed to duplicate seed_nodes string for parsing.");
            return false; // Or handle error differently
        }

        char *token = strtok(str_copy, ", \t\n"); // Split by comma and whitespace
        while (token && cluster_config->seed_node_count < XSAN_MAX_SEED_NODES) {
            char *uuid_part = token;
            char *at_ptr = strchr(token, '@');
            char *ip_part = NULL;
            char *colon_ptr = NULL;
            uint16_t port = 0;

            if (at_ptr) {
                *at_ptr = '\0'; // Null-terminate UUID string
                ip_part = at_ptr + 1;
                colon_ptr = strrchr(ip_part, ':'); // Use strrchr to find the last colon (for IPv6)

                if (colon_ptr) {
                    *colon_ptr = '\0'; // Null-terminate IP string
                    char *port_str = colon_ptr + 1;
                    long long_port = 0;
                    if (xsan_str_to_long(port_str, &long_port) && long_port > 0 && long_port <= 65535) {
                        port = (uint16_t)long_port;
                    } else {
                        XSAN_LOG_WARN("Invalid port string '%s' in seed node entry: %s. Skipping.", port_str, token);
                        token = strtok(NULL, ", \t\n");
                        continue;
                    }
                } else {
                    XSAN_LOG_WARN("Port missing in seed node entry: %s. Skipping.", token);
                    token = strtok(NULL, ", \t\n");
                    continue;
                }
            } else {
                XSAN_LOG_WARN("Invalid format for seed node entry (missing '@'): %s. Skipping.", token);
                token = strtok(NULL, ", \t\n");
                continue;
            }

            xsan_node_t *current_seed_node = &cluster_config->seed_nodes[cluster_config->seed_node_count];
            memset(current_seed_node, 0, sizeof(xsan_node_t)); // Initialize

            // Parse UUID
            if (spdk_uuid_parse((struct spdk_uuid *)&current_seed_node->id.data[0], uuid_part) != 0) {
                XSAN_LOG_WARN("Failed to parse UUID string '%s' for seed node. Skipping entry: %s", uuid_part, token);
                token = strtok(NULL, ", \t\n");
                continue;
            }

            // Copy IP and Port (using storage_addr for data/replication, mgmt_addr could be same or different)
            xsan_strcpy_safe(current_seed_node->storage_addr.ip, ip_part, INET_ADDRSTRLEN);
            current_seed_node->storage_addr.port = port;
            // For simplicity, also copy to hostname and mgmt_addr if needed, or set them distinctly if config supports it
            xsan_strcpy_safe(current_seed_node->hostname, ip_part, XSAN_MAX_NAME_LEN); // Or a configured name if available
            memcpy(&current_seed_node->mgmt_addr, &current_seed_node->storage_addr, sizeof(xsan_address_t));



            char uuid_str[SPDK_UUID_STRING_LEN];
            spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), (struct spdk_uuid*)&current_seed_node->id.data[0]);
            XSAN_LOG_DEBUG("Parsed seed node %zu: ID=%s, IP=%s, Port=%u",
                           cluster_config->seed_node_count,
                           uuid_str,
                           current_seed_node->storage_addr.ip,
                           current_seed_node->storage_addr.port);

            cluster_config->seed_node_count++;
            token = strtok(NULL, ", \t\n");
        }
        xsan_free(str_copy);
    }
    
    cluster_config->min_nodes = xsan_config_get_int(config, "cluster.min_nodes", DEFAULT_MIN_NODES);
    cluster_config->max_nodes = xsan_config_get_int(config, "cluster.max_nodes", DEFAULT_MAX_NODES);
    cluster_config->quorum_size = xsan_config_get_int(config, "cluster.quorum_size", DEFAULT_QUORUM_SIZE);
    cluster_config->network_timeout = xsan_config_get_int(config, "cluster.network_timeout", DEFAULT_NETWORK_TIMEOUT);
    cluster_config->reconnect_interval = xsan_config_get_int(config, "cluster.reconnect_interval", DEFAULT_RECONNECT_INTERVAL);
    cluster_config->enable_auto_failover = xsan_config_get_bool(config, "cluster.enable_auto_failover", true);
    cluster_config->failover_timeout = xsan_config_get_int(config, "cluster.failover_timeout", DEFAULT_FAILOVER_TIMEOUT);
    
    return true;
}

/**
 * 打印配置信息
 */
void xsan_config_print(xsan_config_t *config)
{
    if (!config) {
        return;
    }
    
    XSAN_LOG_INFO("Configuration (%zu items):", config->item_count);
    
    xsan_config_item_t *item = config->items;
    while (item) {
        switch (item->type) {
            case XSAN_CONFIG_TYPE_STRING:
                XSAN_LOG_INFO("  %s = \"%s\"", item->key, item->value.string_val);
                break;
            case XSAN_CONFIG_TYPE_INT:
                XSAN_LOG_INFO("  %s = %d", item->key, item->value.int_val);
                break;
            case XSAN_CONFIG_TYPE_LONG:
                XSAN_LOG_INFO("  %s = %ld", item->key, item->value.long_val);
                break;
            case XSAN_CONFIG_TYPE_DOUBLE:
                XSAN_LOG_INFO("  %s = %f", item->key, item->value.double_val);
                break;
            case XSAN_CONFIG_TYPE_BOOL:
                XSAN_LOG_INFO("  %s = %s", item->key, item->value.bool_val ? "true" : "false");
                break;
        }
        item = item->next;
    }
}

/**
 * 验证配置项
 */
bool xsan_config_validate(xsan_config_t *config)
{
    if (!config) {
        return false;
    }
    
    bool valid = true;
    
    /* 验证必需的配置项 */
    if (!xsan_config_has_key(config, "node.id")) {
        XSAN_LOG_ERROR("Missing required configuration: node.id");
        valid = false;
    }
    
    if (!xsan_config_has_key(config, "node.name")) {
        XSAN_LOG_ERROR("Missing required configuration: node.name");
        valid = false;
    }
    
    /* 验证端口范围 */
    int port = xsan_config_get_int(config, "node.port", 0);
    if (port < 1024 || port > 65535) {
        XSAN_LOG_ERROR("Invalid port number: %d (must be between 1024-65535)", port);
        valid = false;
    }
    
    /* 验证副本数 */
    int replication_factor = xsan_config_get_int(config, "storage.replication_factor", 0);
    if (replication_factor < 1 || replication_factor > 10) {
        XSAN_LOG_ERROR("Invalid replication factor: %d (must be between 1-10)", replication_factor);
        valid = false;
    }
    
    /* 验证仲裁大小 */
    int quorum_size = xsan_config_get_int(config, "cluster.quorum_size", 0);
    if (quorum_size < 1) {
        XSAN_LOG_ERROR("Invalid quorum size: %d (must be at least 1)", quorum_size);
        valid = false;
    }
    
    return valid;
}
