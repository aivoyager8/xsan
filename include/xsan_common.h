#ifndef XSAN_COMMON_H
#define XSAN_COMMON_H

#include "xsan_types.h"
#include <syslog.h>

/* Common utility functions and logging */

/* Logging levels */
typedef enum {
    XSAN_LOG_LEVEL_ERROR = 0,
    XSAN_LOG_LEVEL_WARNING,
    XSAN_LOG_LEVEL_INFO,
    XSAN_LOG_LEVEL_DEBUG
} xsan_log_level_t;

/**
 * Initialize logging subsystem
 * @param log_level Minimum log level to output
 * @param log_file Path to log file (NULL for syslog)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_log_init(xsan_log_level_t log_level, const char *log_file);

/**
 * Shutdown logging subsystem
 */
void xsan_log_shutdown(void);

/**
 * Log a message
 * @param level Log level
 * @param format Printf-style format string
 * @param ... Format arguments
 */
void xsan_log(xsan_log_level_t level, const char *format, ...);

/* Convenience macros for logging */
#define XSAN_LOG_ERROR(fmt, ...) xsan_log(XSAN_LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define XSAN_LOG_WARNING(fmt, ...) xsan_log(XSAN_LOG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define XSAN_LOG_INFO(fmt, ...) xsan_log(XSAN_LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define XSAN_LOG_DEBUG(fmt, ...) xsan_log(XSAN_LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)

/**
 * Generate a new UUID
 * @param uuid Pointer to store the generated UUID
 */
void xsan_uuid_generate(xsan_uuid_t *uuid);

/**
 * Convert UUID to string
 * @param uuid UUID to convert
 * @param str_uuid Buffer to store string representation (minimum 37 bytes)
 */
void xsan_uuid_to_string(const xsan_uuid_t *uuid, char *str_uuid);

/**
 * Parse UUID from string
 * @param str_uuid String representation of UUID
 * @param uuid Pointer to store parsed UUID
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_uuid_from_string(const char *str_uuid, xsan_uuid_t *uuid);

/**
 * Compare two UUIDs
 * @param uuid1 First UUID
 * @param uuid2 Second UUID
 * @return 0 if equal, non-zero if different
 */
int xsan_uuid_compare(const xsan_uuid_t *uuid1, const xsan_uuid_t *uuid2);

/**
 * Check if UUID is null/empty
 * @param uuid UUID to check
 * @return true if null, false otherwise
 */
bool xsan_uuid_is_null(const xsan_uuid_t *uuid);

/**
 * Calculate CRC32 checksum
 * @param data Data to checksum
 * @param size Size of data in bytes
 * @return CRC32 checksum
 */
uint32_t xsan_crc32(const void *data, size_t size);

/**
 * Calculate SHA256 hash
 * @param data Data to hash
 * @param size Size of data in bytes
 * @param hash Buffer to store hash (minimum 32 bytes)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_sha256(const void *data, size_t size, unsigned char *hash);

/**
 * Get current timestamp in microseconds
 * @return Timestamp in microseconds since epoch
 */
uint64_t xsan_get_timestamp_us(void);

/**
 * Get current timestamp in milliseconds
 * @return Timestamp in milliseconds since epoch
 */
uint64_t xsan_get_timestamp_ms(void);

/**
 * Sleep for specified milliseconds
 * @param milliseconds Number of milliseconds to sleep
 */
void xsan_sleep_ms(uint32_t milliseconds);

/**
 * Create a directory recursively
 * @param path Path to create
 * @param mode Directory permissions
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_mkdir_recursive(const char *path, mode_t mode);

/**
 * Check if file exists
 * @param path File path to check
 * @return true if file exists, false otherwise
 */
bool xsan_file_exists(const char *path);

/**
 * Get file size
 * @param path File path
 * @param size Pointer to store file size
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_get_file_size(const char *path, uint64_t *size);

/**
 * Read entire file into memory
 * @param path File path
 * @param data Pointer to store allocated buffer (caller must free)
 * @param size Pointer to store file size
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_read_file(const char *path, void **data, size_t *size);

/**
 * Write data to file
 * @param path File path
 * @param data Data to write
 * @param size Size of data
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_write_file(const char *path, const void *data, size_t size);

/**
 * Get system memory information
 * @param total_mb Pointer to store total memory in MB
 * @param free_mb Pointer to store free memory in MB
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_get_memory_info(uint64_t *total_mb, uint64_t *free_mb);

/**
 * Get CPU information
 * @param cpu_count Pointer to store number of CPU cores
 * @param cpu_mhz Pointer to store CPU frequency in MHz
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_get_cpu_info(uint32_t *cpu_count, uint32_t *cpu_mhz);

/**
 * Get network interface information
 * @param interface Interface name (e.g., "eth0")
 * @param ip_address Buffer to store IP address (minimum INET_ADDRSTRLEN bytes)
 * @param netmask Buffer to store netmask (minimum INET_ADDRSTRLEN bytes)
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_get_network_info(const char *interface, char *ip_address, char *netmask);

/**
 * Convert error code to string
 * @param error_code XSAN error code
 * @return String representation of error code
 */
const char *xsan_error_to_string(xsan_error_t error_code);

/**
 * Safe string copy with bounds checking
 * @param dest Destination buffer
 * @param src Source string
 * @param dest_size Size of destination buffer
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_strcpy_safe(char *dest, const char *src, size_t dest_size);

/**
 * Safe string concatenation with bounds checking
 * @param dest Destination buffer
 * @param src Source string to append
 * @param dest_size Size of destination buffer
 * @return XSAN_SUCCESS on success, error code on failure
 */
xsan_error_t xsan_strcat_safe(char *dest, const char *src, size_t dest_size);

#endif /* XSAN_COMMON_H */
