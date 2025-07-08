#ifndef XSAN_LOG_H
#define XSAN_LOG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include "xsan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Log levels */
typedef enum {
    XSAN_LOG_LEVEL_TRACE = 0,
    XSAN_LOG_LEVEL_DEBUG,
    XSAN_LOG_LEVEL_INFO,
    XSAN_LOG_LEVEL_WARN,
    XSAN_LOG_LEVEL_ERROR,
    XSAN_LOG_LEVEL_FATAL
} xsan_log_level_t;

/* Log configuration */
typedef struct {
    char log_file[XSAN_MAX_PATH_LEN];
    xsan_log_level_t level;
    bool console_output;
    bool file_output;
    size_t max_file_size;
    int max_file_count;
    pthread_mutex_t lock;
    FILE *file_handle;
} xsan_log_config_t;

/**
 * Initialize logging system
 * @param config Log configuration
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_log_init(const xsan_log_config_t *config);

/**
 * Shutdown logging system
 */
void xsan_log_shutdown(void);

/**
 * Log a message
 * @param level Log level
 * @param file Source file name
 * @param line Source line number
 * @param func Function name
 * @param fmt Format string
 * @param ... Arguments
 */
void xsan_log_message(xsan_log_level_t level, const char *file, int line, 
                     const char *func, const char *fmt, ...);

/**
 * Set log level
 * @param level New log level
 */
void xsan_log_set_level(xsan_log_level_t level);

/**
 * Get current log level
 * @return Current log level
 */
xsan_log_level_t xsan_log_get_level(void);

/* Logging macros */
#define XSAN_LOG_TRACE(fmt, ...) \
    xsan_log_message(XSAN_LOG_LEVEL_TRACE, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define XSAN_LOG_DEBUG(fmt, ...) \
    xsan_log_message(XSAN_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define XSAN_LOG_INFO(fmt, ...) \
    xsan_log_message(XSAN_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define XSAN_LOG_WARN(fmt, ...) \
    xsan_log_message(XSAN_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define XSAN_LOG_ERROR(fmt, ...) \
    xsan_log_message(XSAN_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define XSAN_LOG_FATAL(fmt, ...) \
    xsan_log_message(XSAN_LOG_LEVEL_FATAL, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* XSAN_LOG_H */
