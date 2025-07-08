/**
 * XSAN 节点守护进程
 * 
 * 主要功能：
 * 1. 初始化各个模块
 * 2. 启动集群服务
 * 3. 处理命令行参数
 * 4. 信号处理和优雅关闭
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <errno.h>

#include "xsan_error.h"
#include "xsan_log.h"
#include "xsan_types.h"

/* 全局变量 */
static bool g_shutdown_requested = false;
static const char *g_config_file = NULL;
static const char *g_data_dir = "/var/lib/xsan";
static const char *g_log_file = NULL;
static xsan_log_level_t g_log_level = XSAN_LOG_LEVEL_INFO;
static bool g_daemonize = false;
static bool g_version_only = false;

/* 函数声明 */
static void print_usage(const char *program_name);
static void print_version(void);
static void signal_handler(int sig);
static int parse_arguments(int argc, char *argv[]);
static int setup_logging(void);
static int setup_directories(void);
static int daemonize_process(void);
static int initialize_modules(void);
static void cleanup_modules(void);
static int main_loop(void);

/**
 * 打印使用帮助
 */
static void print_usage(const char *program_name)
{
    printf("Usage: %s [OPTIONS]\n", program_name);
    printf("\n");
    printf("XSAN Distributed Storage System - Node Daemon\n");
    printf("\n");
    printf("Options:\n");
    printf("  -c, --config FILE       Configuration file path\n");
    printf("  -d, --data-dir DIR      Data directory (default: %s)\n", g_data_dir);
    printf("  -l, --log-file FILE     Log file path\n");
    printf("  -L, --log-level LEVEL   Log level (trace|debug|info|warn|error|fatal)\n");
    printf("  -D, --daemon            Run as daemon\n");
    printf("  -v, --version           Show version and exit\n");
    printf("  -h, --help              Show this help and exit\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -c /etc/xsan/node.conf\n", program_name);
    printf("  %s -d /data/xsan -l /var/log/xsan.log -D\n", program_name);
    printf("  %s -L debug\n", program_name);
    printf("\n");
}

/**
 * 打印版本信息
 */
static void print_version(void)
{
    extern const char *xsan_get_version(void);
    extern const char *xsan_get_build_info(void);
    
    printf("XSAN Node Daemon\n");
    printf("Version: %s\n", xsan_get_version());
    printf("Built: %s\n", xsan_get_build_info());
    printf("Copyright (c) 2024 XSAN Project\n");
}

/**
 * 信号处理器
 */
static void signal_handler(int sig)
{
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            XSAN_LOG_INFO("Received signal %d, shutting down...", sig);
            g_shutdown_requested = true;
            break;
        case SIGHUP:
            XSAN_LOG_INFO("Received SIGHUP, reloading configuration...");
            /* TODO: 重新加载配置 */
            break;
        default:
            XSAN_LOG_WARN("Received unknown signal %d", sig);
            break;
    }
}

/**
 * 解析命令行参数
 */
static int parse_arguments(int argc, char *argv[])
{
    static struct option long_options[] = {
        {"config",    required_argument, 0, 'c'},
        {"data-dir",  required_argument, 0, 'd'},
        {"log-file",  required_argument, 0, 'l'},
        {"log-level", required_argument, 0, 'L'},
        {"daemon",    no_argument,       0, 'D'},
        {"version",   no_argument,       0, 'v'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int option_index = 0;
    int c;
    
    while ((c = getopt_long(argc, argv, "c:d:l:L:Dvh", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                g_config_file = optarg;
                break;
            case 'd':
                g_data_dir = optarg;
                break;
            case 'l':
                g_log_file = optarg;
                break;
            case 'L':
                g_log_level = xsan_log_level_from_string(optarg);
                break;
            case 'D':
                g_daemonize = true;
                break;
            case 'v':
                g_version_only = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case '?':
                print_usage(argv[0]);
                return -1;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    
    if (g_version_only) {
        print_version();
        return 0;
    }
    
    return 1;  /* 继续执行 */
}

/**
 * 设置日志系统
 */
static int setup_logging(void)
{
    xsan_log_config_t log_config = {
        .level = g_log_level,
        .console_output = !g_daemonize,
        .file_output = g_log_file != NULL,
        .max_file_size = 100 * 1024 * 1024,  /* 100MB */
        .max_file_count = 10
    };
    
    if (g_log_file) {
        strncpy(log_config.log_file, g_log_file, sizeof(log_config.log_file) - 1);
    }
    
    xsan_error_t err = xsan_log_init(&log_config);
    if (err != XSAN_OK) {
        fprintf(stderr, "Failed to initialize logging: %s\n", xsan_error_string(err));
        return -1;
    }
    
    XSAN_LOG_INFO("XSAN node daemon starting...");
    XSAN_LOG_INFO("Version: %s", xsan_get_version());
    XSAN_LOG_INFO("Data directory: %s", g_data_dir);
    if (g_config_file) {
        XSAN_LOG_INFO("Configuration file: %s", g_config_file);
    }
    
    return 0;
}

/**
 * 设置数据目录
 */
static int setup_directories(void)
{
    struct stat st;
    
    /* 检查数据目录是否存在 */
    if (stat(g_data_dir, &st) != 0) {
        if (errno == ENOENT) {
            XSAN_LOG_INFO("Creating data directory: %s", g_data_dir);
            if (mkdir(g_data_dir, 0755) != 0) {
                XSAN_LOG_ERROR("Failed to create data directory: %s", strerror(errno));
                return -1;
            }
        } else {
            XSAN_LOG_ERROR("Failed to check data directory: %s", strerror(errno));
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
        XSAN_LOG_ERROR("Data directory is not a directory: %s", g_data_dir);
        return -1;
    }
    
    /* 检查权限 */
    if (access(g_data_dir, R_OK | W_OK) != 0) {
        XSAN_LOG_ERROR("No read/write permission for data directory: %s", g_data_dir);
        return -1;
    }
    
    return 0;
}

/**
 * 后台化进程
 */
static int daemonize_process(void)
{
    if (!g_daemonize) {
        return 0;
    }
    
    XSAN_LOG_INFO("Daemonizing process...");
    
    /* 创建子进程 */
    pid_t pid = fork();
    if (pid < 0) {
        XSAN_LOG_ERROR("Failed to fork: %s", strerror(errno));
        return -1;
    }
    
    if (pid > 0) {
        /* 父进程退出 */
        exit(0);
    }
    
    /* 子进程继续 */
    /* 创建新的会话 */
    if (setsid() < 0) {
        XSAN_LOG_ERROR("Failed to create new session: %s", strerror(errno));
        return -1;
    }
    
    /* 再次fork以确保不是会话组长 */
    pid = fork();
    if (pid < 0) {
        XSAN_LOG_ERROR("Failed to fork again: %s", strerror(errno));
        return -1;
    }
    
    if (pid > 0) {
        /* 父进程退出 */
        exit(0);
    }
    
    /* 改变工作目录 */
    if (chdir("/") < 0) {
        XSAN_LOG_ERROR("Failed to change directory: %s", strerror(errno));
        return -1;
    }
    
    /* 设置文件权限掩码 */
    umask(0);
    
    /* 关闭标准输入输出 */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    return 0;
}

/**
 * 初始化各个模块
 */
static int initialize_modules(void)
{
    XSAN_LOG_INFO("Initializing modules...");
    
    /* TODO: 初始化各个模块 */
    /* 
     * 1. 初始化存储模块
     * 2. 初始化网络模块
     * 3. 初始化集群模块
     * 4. 初始化复制模块
     * 5. 初始化策略模块
     * 6. 初始化虚拟化模块
     */
    
    XSAN_LOG_INFO("All modules initialized successfully");
    return 0;
}

/**
 * 清理模块
 */
static void cleanup_modules(void)
{
    XSAN_LOG_INFO("Cleaning up modules...");
    
    /* TODO: 清理各个模块 */
    /* 按初始化的逆序清理 */
    
    XSAN_LOG_INFO("All modules cleaned up");
}

/**
 * 主循环
 */
static int main_loop(void)
{
    XSAN_LOG_INFO("Entering main loop...");
    
    while (!g_shutdown_requested) {
        /* TODO: 主循环逻辑 */
        /* 
         * 1. 处理网络事件
         * 2. 处理存储请求
         * 3. 处理集群事件
         * 4. 进行健康检查
         */
        
        /* 暂时简单地睡眠 */
        sleep(1);
    }
    
    XSAN_LOG_INFO("Main loop exited");
    return 0;
}

/**
 * 主函数
 */
int main(int argc, char *argv[])
{
    int ret = 0;
    
    /* 解析命令行参数 */
    int parse_result = parse_arguments(argc, argv);
    if (parse_result <= 0) {
        return parse_result == 0 ? 0 : 1;
    }
    
    /* 设置日志系统 */
    if (setup_logging() != 0) {
        return 1;
    }
    
    /* 设置信号处理器 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    
    /* 设置数据目录 */
    if (setup_directories() != 0) {
        ret = 1;
        goto cleanup;
    }
    
    /* 后台化进程 */
    if (daemonize_process() != 0) {
        ret = 1;
        goto cleanup;
    }
    
    /* 初始化模块 */
    if (initialize_modules() != 0) {
        ret = 1;
        goto cleanup;
    }
    
    /* 进入主循环 */
    if (main_loop() != 0) {
        ret = 1;
    }
    
cleanup:
    /* 清理模块 */
    cleanup_modules();
    
    /* 关闭日志系统 */
    XSAN_LOG_INFO("XSAN node daemon shutting down");
    xsan_log_shutdown();
    
    return ret;
}
