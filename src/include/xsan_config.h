#ifndef XSAN_MAX_NAME_LEN
#define XSAN_MAX_NAME_LEN 128
#endif
#ifndef XSAN_MAX_NAME_LEN
#define XSAN_MAX_NAME_LEN 128
#endif
/**
 * XSAN 配置管理模块
 * 
 * 提供配置文件解析和配置参数管理功能
 */

#ifndef XSAN_CONFIG_H
#define XSAN_CONFIG_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 配置值类型 */
typedef enum {
    XSAN_CONFIG_TYPE_STRING,
    XSAN_CONFIG_TYPE_INT,
    XSAN_CONFIG_TYPE_LONG,
    XSAN_CONFIG_TYPE_DOUBLE,
    XSAN_CONFIG_TYPE_BOOL
} xsan_config_type_t;

/* 配置项结构 */
typedef struct xsan_config_item {
    char *key;                      /* 配置键 */
    xsan_config_type_t type;        /* 值类型 */
    union {
        char *string_val;
        int int_val;
        long long_val;
        double double_val;
        bool bool_val;
    } value;                        /* 配置值 */
    struct xsan_config_item *next;  /* 链表指针 */
} xsan_config_item_t;

/* 配置管理器 */
typedef struct xsan_config {
    xsan_config_item_t *items;      /* 配置项链表 */
    size_t item_count;              /* 配置项数量 */
    char *config_file;              /* 配置文件路径 */
    bool auto_reload;               /* 是否自动重载 */
    time_t last_modified;           /* 最后修改时间 */
} xsan_config_t;

/* 节点配置 */
typedef struct xsan_node_config {
    char node_id[64];               /* 节点ID */
    char node_name[128];            /* 节点名称 */
    char bind_address[64];          /* 绑定地址 */
    uint16_t port;                  /* 端口号 */
    char data_dir[256];             /* 数据目录 */
    char log_file[256];             /* 日志文件 */
    char log_level[16];             /* 日志级别 */
    size_t max_connections;         /* 最大连接数 */
    size_t heartbeat_interval;      /* 心跳间隔(秒) */
    size_t election_timeout;        /* 选举超时(秒) */
    bool enable_ssl;                /* 是否启用SSL */
    char ssl_cert_file[256];        /* SSL证书文件 */
    char ssl_key_file[256];         /* SSL私钥文件 */
    char nvmf_target_nqn[XSAN_MAX_NAME_LEN]; /* Optional NQN for NVMe-oF target */
    char nvmf_listen_port[16];        /* Optional NVMe-oF listen port (as string) */
} xsan_node_config_t;

/* 存储配置 */
typedef struct xsan_storage_config {
    char storage_dir[256];          /* 存储目录 */
    size_t block_size;              /* 块大小 */
    size_t max_file_size;           /* 最大文件大小 */
    size_t cache_size;              /* 缓存大小 */
    size_t io_threads;              /* IO线程数 */
    size_t sync_interval;           /* 同步间隔(秒) */
    bool enable_compression;        /* 是否启用压缩 */
    char compression_algorithm[32]; /* 压缩算法 */
    size_t replication_factor;      /* 副本数 */
    bool enable_checksums;          /* 是否启用校验和 */
} xsan_storage_config_t;

#include "xsan_types.h" // For xsan_node_t, xsan_uuid_t

#define XSAN_MAX_SEED_NODES 32 // Define max seed nodes for config

/* 集群配置 */
typedef struct xsan_cluster_config {
    char cluster_name[128];         /* 集群名称 */
    xsan_node_t seed_nodes[XSAN_MAX_SEED_NODES]; /* 解析后的种子节点信息 */
    size_t seed_node_count;         /* 种子节点数量 */
    size_t min_nodes;               /* 最小节点数 */
    size_t max_nodes;               /* 最大节点数 */
    size_t quorum_size;             /* 仲裁大小 */
    size_t network_timeout;         /* 网络超时(秒) */
    size_t reconnect_interval;      /* 重连间隔(秒) */
    bool enable_auto_failover;      /* 是否启用自动故障转移 */
    size_t failover_timeout;        /* 故障转移超时(秒) */
} xsan_cluster_config_t;

/**
 * 创建配置管理器
 * 
 * @return 配置管理器句柄，失败返回NULL
 */
xsan_config_t *xsan_config_create(void);

/**
 * 销毁配置管理器
 * 
 * @param config 配置管理器句柄
 */
void xsan_config_destroy(xsan_config_t *config);

/**
 * 从文件加载配置
 * 
 * @param config 配置管理器句柄
 * @param config_file 配置文件路径
 * @return 是否加载成功
 */
bool xsan_config_load_from_file(xsan_config_t *config, const char *config_file);

/**
 * 从字符串加载配置
 * 
 * @param config 配置管理器句柄
 * @param config_str 配置字符串
 * @return 是否加载成功
 */
bool xsan_config_load_from_string(xsan_config_t *config, const char *config_str);

/**
 * 保存配置到文件
 * 
 * @param config 配置管理器句柄
 * @param config_file 配置文件路径
 * @return 是否保存成功
 */
bool xsan_config_save_to_file(xsan_config_t *config, const char *config_file);

/**
 * 设置字符串配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param value 配置值
 * @return 是否设置成功
 */
bool xsan_config_set_string(xsan_config_t *config, const char *key, const char *value);

/**
 * 设置整数配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param value 配置值
 * @return 是否设置成功
 */
bool xsan_config_set_int(xsan_config_t *config, const char *key, int value);

/**
 * 设置长整数配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param value 配置值
 * @return 是否设置成功
 */
bool xsan_config_set_long(xsan_config_t *config, const char *key, long value);

/**
 * 设置双精度浮点数配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param value 配置值
 * @return 是否设置成功
 */
bool xsan_config_set_double(xsan_config_t *config, const char *key, double value);

/**
 * 设置布尔配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param value 配置值
 * @return 是否设置成功
 */
bool xsan_config_set_bool(xsan_config_t *config, const char *key, bool value);

/**
 * 获取字符串配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param default_value 默认值
 * @return 配置值
 */
const char *xsan_config_get_string(xsan_config_t *config, const char *key, const char *default_value);

/**
 * 获取整数配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param default_value 默认值
 * @return 配置值
 */
int xsan_config_get_int(xsan_config_t *config, const char *key, int default_value);

/**
 * 获取长整数配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param default_value 默认值
 * @return 配置值
 */
long xsan_config_get_long(xsan_config_t *config, const char *key, long default_value);

/**
 * 获取双精度浮点数配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param default_value 默认值
 * @return 配置值
 */
double xsan_config_get_double(xsan_config_t *config, const char *key, double default_value);

/**
 * 获取布尔配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @param default_value 默认值
 * @return 配置值
 */
bool xsan_config_get_bool(xsan_config_t *config, const char *key, bool default_value);

/**
 * 检查配置项是否存在
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @return 是否存在
 */
bool xsan_config_has_key(xsan_config_t *config, const char *key);

/**
 * 删除配置项
 * 
 * @param config 配置管理器句柄
 * @param key 配置键
 * @return 是否删除成功
 */
bool xsan_config_remove_key(xsan_config_t *config, const char *key);

/**
 * 清空所有配置项
 * 
 * @param config 配置管理器句柄
 */
void xsan_config_clear(xsan_config_t *config);

/**
 * 获取配置项数量
 * 
 * @param config 配置管理器句柄
 * @return 配置项数量
 */
size_t xsan_config_get_item_count(xsan_config_t *config);

/**
 * 检查配置文件是否已修改
 * 
 * @param config 配置管理器句柄
 * @return 是否已修改
 */
bool xsan_config_is_modified(xsan_config_t *config);

/**
 * 重载配置文件
 * 
 * @param config 配置管理器句柄
 * @return 是否重载成功
 */
bool xsan_config_reload(xsan_config_t *config);

/**
 * 设置自动重载
 * 
 * @param config 配置管理器句柄
 * @param auto_reload 是否自动重载
 */
void xsan_config_set_auto_reload(xsan_config_t *config, bool auto_reload);

/**
 * 从配置管理器加载节点配置
 * 
 * @param config 配置管理器句柄
 * @param node_config 节点配置结构
 * @return 是否加载成功
 */
bool xsan_config_load_node_config(xsan_config_t *config, xsan_node_config_t *node_config);

/**
 * 从配置管理器加载存储配置
 * 
 * @param config 配置管理器句柄
 * @param storage_config 存储配置结构
 * @return 是否加载成功
 */
bool xsan_config_load_storage_config(xsan_config_t *config, xsan_storage_config_t *storage_config);

/**
 * 从配置管理器加载集群配置
 * 
 * @param config 配置管理器句柄
 * @param cluster_config 集群配置结构
 * @return 是否加载成功
 */
bool xsan_config_load_cluster_config(xsan_config_t *config, xsan_cluster_config_t *cluster_config);

/**
 * 打印配置信息
 * 
 * @param config 配置管理器句柄
 */
void xsan_config_print(xsan_config_t *config);

/**
 * 验证配置项
 * 
 * @param config 配置管理器句柄
 * @return 是否验证成功
 */
bool xsan_config_validate(xsan_config_t *config);

#ifdef __cplusplus
}
#endif

#endif /* XSAN_CONFIG_H */
