/**
 * XSAN 版本信息实现
 */

#include "xsan_types.h" // For uint32_t, etc. Potentially include xsan.h if it has the canonical declaration
#include <stdio.h>
#include <string.h>

/* 版本信息定义 */
#define XSAN_VERSION_MAJOR 1
#define XSAN_VERSION_MINOR 0
#define XSAN_VERSION_PATCH 0
#define XSAN_BUILD_INFO_STR __DATE__ " " __TIME__

// This can be generated dynamically or kept static if preferred.
// For simplicity, we'll use the macros.
// static const char *xsan_version_full_string = "1.0.0"; // Example, could be dynamic

static const char *g_xsan_build_info_str = XSAN_BUILD_INFO_STR;

/**
 * Get XSAN version information
 * This function now matches the declaration in xsan.h.
 * @param major Pointer to store major version (uint32_t)
 * @param minor Pointer to store minor version (uint32_t)
 * @param patch Pointer to store patch version (uint32_t)
 * @param build_string Pointer to store build string (const char **)
 */
void xsan_get_version(uint32_t *major, uint32_t *minor, uint32_t *patch, const char **build_string)
{
    if (major) {
        *major = XSAN_VERSION_MAJOR;
    }
    if (minor) {
        *minor = XSAN_VERSION_MINOR;
    }
    if (patch) {
        *patch = XSAN_VERSION_PATCH;
    }
    if (build_string) {
        *build_string = g_xsan_build_info_str;
    }
}

/**
 * 获取版本字符串 (e.g., "1.0.0") - kept for internal use or if needed elsewhere.
 * If xsan.h only exposes the combined one, this might be removed or made static.
 */
const char *xsan_get_version_string(void)
{
    // This could be a static string or dynamically generated
    // For now, let's assume a simple static string.
    // If dynamic generation is needed:
    // static char version_str_buf[32];
    // snprintf(version_str_buf, sizeof(version_str_buf), "%d.%d.%d", XSAN_VERSION_MAJOR, XSAN_VERSION_MINOR, XSAN_VERSION_PATCH);
    // return version_str_buf;
    return "1.0.0"; // Placeholder if this specific format string is still needed
}

/**
 * 获取构建信息 - kept for internal use or if needed elsewhere.
 */
const char *xsan_get_build_info_string(void)
{
    return g_xsan_build_info_str;
}

/**
 * 打印版本信息
 */
void xsan_print_version(void)
{
    uint32_t major, minor, patch;
    const char *build_str;

    xsan_get_version(&major, &minor, &patch, &build_str);

    printf("XSAN Distributed Storage System\n");
    printf("Version: %u.%u.%u\n", major, minor, patch);
    printf("Built: %s\n", build_str ? build_str : "N/A");
    printf("Copyright (c) 2024 XSAN Project\n");
}
