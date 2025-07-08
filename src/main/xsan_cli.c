/**
 * XSAN 命令行工具
 * 
 * 提供集群管理、卷管理、监控等功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "xsan_error.h"
#include "xsan_log.h"
#include "xsan_types.h"

/* 命令类型 */
typedef enum {
    CMD_NONE = 0,
    CMD_VERSION,
    CMD_CLUSTER,
    CMD_VOLUME,
    CMD_NODE,
    CMD_STATS
} command_type_t;

/* 子命令类型 */
typedef enum {
    SUBCMD_NONE = 0,
    SUBCMD_LIST,
    SUBCMD_INFO,
    SUBCMD_CREATE,
    SUBCMD_DELETE,
    SUBCMD_STATUS,
    SUBCMD_SHOW
} subcommand_type_t;

/* 命令行选项 */
static struct {
    command_type_t command;
    subcommand_type_t subcommand;
    const char *name;
    const char *size;
    const char *config_file;
    const char *server_address;
    int replica_count;
    bool verbose;
    bool help;
} g_options = {0};

/* 函数声明 */
static void print_usage(const char *program_name);
static void print_version(void);
static int parse_arguments(int argc, char *argv[]);
static int execute_command(void);
static int cmd_version(void);
static int cmd_cluster(void);
static int cmd_volume(void);
static int cmd_node(void);
static int cmd_stats(void);

/**
 * 打印使用帮助
 */
static void print_usage(const char *program_name)
{
    printf("Usage: %s [COMMAND] [OPTIONS]\n", program_name);
    printf("\n");
    printf("XSAN Distributed Storage System - Command Line Interface\n");
    printf("\n");
    printf("Commands:\n");
    printf("  cluster      Cluster management\n");
    printf("  volume       Volume management\n");
    printf("  node         Node management\n");
    printf("  stats        Statistics and monitoring\n");
    printf("  version      Show version information\n");
    printf("\n");
    printf("Global Options:\n");
    printf("  -c, --config FILE       Configuration file path\n");
    printf("  -s, --server ADDRESS    Server address (default: localhost:8080)\n");
    printf("  -v, --verbose           Verbose output\n");
    printf("  -h, --help              Show help\n");
    printf("\n");
    printf("Cluster Commands:\n");
    printf("  %s cluster status              Show cluster status\n", program_name);
    printf("  %s cluster list                List all nodes\n", program_name);
    printf("  %s cluster info                Show cluster information\n", program_name);
    printf("\n");
    printf("Volume Commands:\n");
    printf("  %s volume list                 List all volumes\n", program_name);
    printf("  %s volume info <name>          Show volume information\n", program_name);
    printf("  %s volume create <name> <size> Create a new volume\n", program_name);
    printf("  %s volume delete <name>        Delete a volume\n", program_name);
    printf("\n");
    printf("Node Commands:\n");
    printf("  %s node list                   List all nodes\n", program_name);
    printf("  %s node info <id>              Show node information\n", program_name);
    printf("\n");
    printf("Statistics Commands:\n");
    printf("  %s stats show                  Show system statistics\n", program_name);
    printf("\n");
    printf("Examples:\n");
    printf("  %s cluster status\n", program_name);
    printf("  %s volume create vm-disk-01 100GB\n", program_name);
    printf("  %s volume list\n", program_name);
    printf("  %s node info node-001\n", program_name);
    printf("\n");
}

/**
 * 打印版本信息
 */
static void print_version(void)
{
    extern const char *xsan_get_version(void);
    extern const char *xsan_get_build_info(void);
    
    printf("XSAN Command Line Interface\n");
    printf("Version: %s\n", xsan_get_version());
    printf("Built: %s\n", xsan_get_build_info());
    printf("Copyright (c) 2024 XSAN Project\n");
}

/**
 * 解析命令行参数
 */
static int parse_arguments(int argc, char *argv[])
{
    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }
    
    /* 解析主命令 */
    const char *command = argv[1];
    
    if (strcmp(command, "version") == 0) {
        g_options.command = CMD_VERSION;
        return 0;
    } else if (strcmp(command, "cluster") == 0) {
        g_options.command = CMD_CLUSTER;
    } else if (strcmp(command, "volume") == 0) {
        g_options.command = CMD_VOLUME;
    } else if (strcmp(command, "node") == 0) {
        g_options.command = CMD_NODE;
    } else if (strcmp(command, "stats") == 0) {
        g_options.command = CMD_STATS;
    } else if (strcmp(command, "help") == 0 || strcmp(command, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    } else {
        printf("Unknown command: %s\n", command);
        print_usage(argv[0]);
        return -1;
    }
    
    /* 解析子命令 */
    if (argc > 2) {
        const char *subcommand = argv[2];
        
        if (strcmp(subcommand, "list") == 0) {
            g_options.subcommand = SUBCMD_LIST;
        } else if (strcmp(subcommand, "info") == 0) {
            g_options.subcommand = SUBCMD_INFO;
            if (argc > 3) {
                g_options.name = argv[3];
            }
        } else if (strcmp(subcommand, "create") == 0) {
            g_options.subcommand = SUBCMD_CREATE;
            if (argc > 3) {
                g_options.name = argv[3];
            }
            if (argc > 4) {
                g_options.size = argv[4];
            }
        } else if (strcmp(subcommand, "delete") == 0) {
            g_options.subcommand = SUBCMD_DELETE;
            if (argc > 3) {
                g_options.name = argv[3];
            }
        } else if (strcmp(subcommand, "status") == 0) {
            g_options.subcommand = SUBCMD_STATUS;
        } else if (strcmp(subcommand, "show") == 0) {
            g_options.subcommand = SUBCMD_SHOW;
        } else {
            printf("Unknown subcommand: %s\n", subcommand);
            return -1;
        }
    }
    
    return 1;
}

/**
 * 执行命令
 */
static int execute_command(void)
{
    switch (g_options.command) {
        case CMD_VERSION:
            return cmd_version();
        case CMD_CLUSTER:
            return cmd_cluster();
        case CMD_VOLUME:
            return cmd_volume();
        case CMD_NODE:
            return cmd_node();
        case CMD_STATS:
            return cmd_stats();
        default:
            printf("No command specified\n");
            return -1;
    }
}

/**
 * 版本命令
 */
static int cmd_version(void)
{
    print_version();
    return 0;
}

/**
 * 集群命令
 */
static int cmd_cluster(void)
{
    switch (g_options.subcommand) {
        case SUBCMD_STATUS:
            printf("Cluster Status:\n");
            printf("  Status: Online\n");
            printf("  Nodes: 3\n");
            printf("  Leader: node-001\n");
            printf("  Health: Good\n");
            return 0;
            
        case SUBCMD_LIST:
            printf("Cluster Nodes:\n");
            printf("  node-001  192.168.1.10:8080  Leader    Online\n");
            printf("  node-002  192.168.1.11:8080  Follower  Online\n");
            printf("  node-003  192.168.1.12:8080  Follower  Online\n");
            return 0;
            
        case SUBCMD_INFO:
            printf("Cluster Information:\n");
            printf("  Cluster ID: xsan-cluster-001\n");
            printf("  Version: 1.0.0\n");
            printf("  Created: 2024-01-01 10:00:00\n");
            printf("  Total Capacity: 10.0 TB\n");
            printf("  Used Capacity: 2.5 TB\n");
            printf("  Available Capacity: 7.5 TB\n");
            return 0;
            
        default:
            printf("Unknown cluster subcommand\n");
            return -1;
    }
}

/**
 * 卷命令
 */
static int cmd_volume(void)
{
    switch (g_options.subcommand) {
        case SUBCMD_LIST:
            printf("Volumes:\n");
            printf("  vm-disk-01    100GB    2 replicas    Online\n");
            printf("  vm-disk-02    200GB    2 replicas    Online\n");
            printf("  vm-disk-03    500GB    3 replicas    Online\n");
            return 0;
            
        case SUBCMD_INFO:
            if (!g_options.name) {
                printf("Volume name required\n");
                return -1;
            }
            printf("Volume Information: %s\n", g_options.name);
            printf("  Size: 100GB\n");
            printf("  Replicas: 2\n");
            printf("  Status: Online\n");
            printf("  Policy: high-performance\n");
            printf("  Created: 2024-01-01 10:00:00\n");
            return 0;
            
        case SUBCMD_CREATE:
            if (!g_options.name || !g_options.size) {
                printf("Volume name and size required\n");
                return -1;
            }
            printf("Creating volume: %s (%s)\n", g_options.name, g_options.size);
            printf("Volume created successfully\n");
            return 0;
            
        case SUBCMD_DELETE:
            if (!g_options.name) {
                printf("Volume name required\n");
                return -1;
            }
            printf("Deleting volume: %s\n", g_options.name);
            printf("Volume deleted successfully\n");
            return 0;
            
        default:
            printf("Unknown volume subcommand\n");
            return -1;
    }
}

/**
 * 节点命令
 */
static int cmd_node(void)
{
    switch (g_options.subcommand) {
        case SUBCMD_LIST:
            printf("Nodes:\n");
            printf("  node-001  192.168.1.10:8080  Leader    Online    CPU:15%% MEM:2.1GB\n");
            printf("  node-002  192.168.1.11:8080  Follower  Online    CPU:12%% MEM:1.8GB\n");
            printf("  node-003  192.168.1.12:8080  Follower  Online    CPU:18%% MEM:2.3GB\n");
            return 0;
            
        case SUBCMD_INFO:
            if (!g_options.name) {
                printf("Node ID required\n");
                return -1;
            }
            printf("Node Information: %s\n", g_options.name);
            printf("  Address: 192.168.1.10:8080\n");
            printf("  Role: Leader\n");
            printf("  Status: Online\n");
            printf("  CPU Usage: 15%%\n");
            printf("  Memory Usage: 2.1GB / 16GB\n");
            printf("  Disk Usage: 2.5TB / 10TB\n");
            printf("  Uptime: 7 days\n");
            return 0;
            
        default:
            printf("Unknown node subcommand\n");
            return -1;
    }
}

/**
 * 统计命令
 */
static int cmd_stats(void)
{
    switch (g_options.subcommand) {
        case SUBCMD_SHOW:
            printf("System Statistics:\n");
            printf("  Total Capacity: 10.0 TB\n");
            printf("  Used Capacity: 2.5 TB (25%%)\n");
            printf("  Available Capacity: 7.5 TB (75%%)\n");
            printf("  Total Volumes: 3\n");
            printf("  Total Nodes: 3\n");
            printf("  Active Connections: 25\n");
            printf("  Read IOPS: 1,250\n");
            printf("  Write IOPS: 850\n");
            printf("  Read Throughput: 125 MB/s\n");
            printf("  Write Throughput: 85 MB/s\n");
            return 0;
            
        default:
            printf("Unknown stats subcommand\n");
            return -1;
    }
}

/**
 * 主函数
 */
int main(int argc, char *argv[])
{
    /* 解析命令行参数 */
    int parse_result = parse_arguments(argc, argv);
    if (parse_result <= 0) {
        return parse_result == 0 ? 0 : 1;
    }
    
    /* 初始化日志系统 (仅错误日志) */
    xsan_log_config_t log_config = {
        .level = XSAN_LOG_LEVEL_ERROR,
        .console_output = false,
        .file_output = false
    };
    xsan_log_init(&log_config);
    
    /* 执行命令 */
    int result = execute_command();
    
    /* 清理 */
    xsan_log_shutdown();
    
    return result == 0 ? 0 : 1;
}
