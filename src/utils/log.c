/**
 * XSAN 日志系统实现
 * 
 * 提供线程安全的日志记录功能，支持文件和控制台输出
 */

#include "xsan_log.h"
#include "xsan_error.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* 全局日志配置 */
static xsan_log_config_t g_log_config = {
    .log_file = "",
    .level = XSAN_LOG_LEVEL_INFO,
    .console_output = true,
    .file_output = false,
    .max_file_size = 100 * 1024 * 1024,  /* 100MB */
    .max_file_count = 10,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .file_handle = NULL
};

static bool g_log_initialized = false;

/* 日志级别字符串 */
static const char *log_level_strings[] = {
    "TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"
};

/* 日志级别颜色 (ANSI 颜色码) */
static const char *log_level_colors[] = {
    "\033[37m",  /* TRACE - 白色 */
    "\033[36m",  /* DEBUG - 青色 */
    "\033[32m",  /* INFO - 绿色 */
    "\033[33m",  /* WARN - 黄色 */
    "\033[31m",  /* ERROR - 红色 */
    "\033[35m"   /* FATAL - 紫色 */
};

static const char *color_reset = "\033[0m";

/**
 * 获取当前时间字符串
 */
static void get_timestamp(char *buffer, size_t size)
{
    time_t now;
    struct tm *tm_info;
    struct timespec ts;
    
    clock_gettime(CLOCK_REALTIME, &ts);
    now = ts.tv_sec;
    tm_info = localtime(&now);
    
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
             ts.tv_nsec / 1000000);
}

/**
 * 轮转日志文件
 */
static void rotate_log_file(void)
{
    char old_path[XSAN_MAX_PATH_LEN];
    char new_path[XSAN_MAX_PATH_LEN];
    
    /* 关闭当前文件 */
    if (g_log_config.file_handle) {
        fclose(g_log_config.file_handle);
        g_log_config.file_handle = NULL;
    }
    
    /* 轮转文件 */
    for (int i = g_log_config.max_file_count - 1; i > 0; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", g_log_config.log_file, i - 1);
        snprintf(new_path, sizeof(new_path), "%s.%d", g_log_config.log_file, i);
        
        if (i == 1) {
            snprintf(old_path, sizeof(old_path), "%s", g_log_config.log_file);
        }
        
        rename(old_path, new_path);
    }
    
    /* 重新打开文件 */
    g_log_config.file_handle = fopen(g_log_config.log_file, "w");
}

/**
 * 检查是否需要轮转日志文件
 */
static void check_log_rotation(void)
{
    if (!g_log_config.file_handle) {
        return;
    }
    
    struct stat st;
    if (stat(g_log_config.log_file, &st) == 0) {
        if (st.st_size >= g_log_config.max_file_size) {
            rotate_log_file();
        }
    }
}

/**
 * 初始化日志系统
 */
xsan_error_t xsan_log_init(const xsan_log_config_t *config)
{
    if (!config) {
        return XSAN_ERROR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&g_log_config.lock);
    
    /* 复制配置 */
    g_log_config.level = config->level;
    g_log_config.console_output = config->console_output;
    g_log_config.file_output = config->file_output;
    g_log_config.max_file_size = config->max_file_size;
    g_log_config.max_file_count = config->max_file_count;
    
    if (config->file_output && strlen(config->log_file) > 0) {
        strncpy(g_log_config.log_file, config->log_file, sizeof(g_log_config.log_file) - 1);
        g_log_config.log_file[sizeof(g_log_config.log_file) - 1] = '\0';
        
        /* 打开日志文件 */
        g_log_config.file_handle = fopen(g_log_config.log_file, "a");
        if (!g_log_config.file_handle) {
            pthread_mutex_unlock(&g_log_config.lock);
            return XSAN_ERROR_IO;
        }
    }
    
    g_log_initialized = true;
    pthread_mutex_unlock(&g_log_config.lock);
    
    return XSAN_OK;
}

/**
 * 关闭日志系统
 */
void xsan_log_shutdown(void)
{
    pthread_mutex_lock(&g_log_config.lock);
    
    if (g_log_config.file_handle) {
        fclose(g_log_config.file_handle);
        g_log_config.file_handle = NULL;
    }
    
    g_log_initialized = false;
    pthread_mutex_unlock(&g_log_config.lock);
}

/**
 * 设置日志级别
 */
void xsan_log_set_level(xsan_log_level_t level)
{
    if (level >= XSAN_LOG_LEVEL_TRACE && level <= XSAN_LOG_LEVEL_FATAL) {
        g_log_config.level = level;
    }
}

/**
 * 获取当前日志级别
 */
xsan_log_level_t xsan_log_get_level(void)
{
    return g_log_config.level;
}

/**
 * 记录日志消息
 */
void xsan_log_message(xsan_log_level_t level, const char *file, int line, 
                     const char *func, const char *fmt, ...)
{
    if (!g_log_initialized || level < g_log_config.level) {
        return;
    }
    
    va_list args;
    va_start(args, fmt);
    
    pthread_mutex_lock(&g_log_config.lock);
    
    /* 准备日志消息 */
    char timestamp[32];
    char message[2048];
    char full_message[4096];
    
    get_timestamp(timestamp, sizeof(timestamp));
    vsnprintf(message, sizeof(message), fmt, args);
    
    /* 提取文件名 */
    const char *filename = strrchr(file, '/');
    if (filename) {
        filename++;
    } else {
        filename = file;
    }
    
    /* 格式化完整消息 */
    snprintf(full_message, sizeof(full_message),
             "[%s] [%s] [%s:%d] [%s] %s\n",
             timestamp, log_level_strings[level], filename, line, func, message);
    
    /* 输出到控制台 */
    if (g_log_config.console_output) {
        bool use_color = isatty(STDERR_FILENO);
        
        if (use_color) {
            fprintf(stderr, "%s%s%s", log_level_colors[level], full_message, color_reset);
        } else {
            fprintf(stderr, "%s", full_message);
        }
        fflush(stderr);
    }
    
    /* 输出到文件 */
    if (g_log_config.file_output && g_log_config.file_handle) {
        check_log_rotation();
        
        if (g_log_config.file_handle) {
            fprintf(g_log_config.file_handle, "%s", full_message);
            fflush(g_log_config.file_handle);
        }
    }
    
    pthread_mutex_unlock(&g_log_config.lock);
    va_end(args);
}

/**
 * 获取日志级别字符串
 */
const char *xsan_log_level_string(xsan_log_level_t level)
{
    if (level >= XSAN_LOG_LEVEL_TRACE && level <= XSAN_LOG_LEVEL_FATAL) {
        return log_level_strings[level];
    }
    return "UNKNOWN";
}

/**
 * 从字符串解析日志级别
 */
xsan_log_level_t xsan_log_level_from_string(const char *str)
{
    if (!str) {
        return XSAN_LOG_LEVEL_INFO;
    }
    
    for (int i = 0; i < sizeof(log_level_strings) / sizeof(log_level_strings[0]); i++) {
        if (strcasecmp(str, log_level_strings[i]) == 0) {
            return (xsan_log_level_t)i;
        }
    }
    
    return XSAN_LOG_LEVEL_INFO;
}

/**
 * 刷新日志缓冲区
 */
void xsan_log_flush(void)
{
    pthread_mutex_lock(&g_log_config.lock);
    
    if (g_log_config.console_output) {
        fflush(stderr);
    }
    
    if (g_log_config.file_output && g_log_config.file_handle) {
        fflush(g_log_config.file_handle);
    }
    
    pthread_mutex_unlock(&g_log_config.lock);
}

/**
 * 检查某个级别的日志是否启用
 */
bool xsan_log_is_enabled(xsan_log_level_t level)
{
    return g_log_initialized && level >= g_log_config.level;
}

/**
 * 创建默认日志配置
 */
xsan_log_config_t xsan_log_default_config(void)
{
    xsan_log_config_t config = {
        .log_file = "",
        .level = XSAN_LOG_LEVEL_INFO,
        .console_output = true,
        .file_output = false,
        .max_file_size = 100 * 1024 * 1024,  /* 100MB */
        .max_file_count = 10,
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .file_handle = NULL
    };
    
    return config;
}
