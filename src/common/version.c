/**
 * XSAN 版本信息实现
 */

#include "xsan_types.h"
#include <stdio.h>
#include <string.h>

/* 版本信息 */
#define XSAN_VERSION_MAJOR 1
#define XSAN_VERSION_MINOR 0
#define XSAN_VERSION_PATCH 0
#define XSAN_VERSION_BUILD __DATE__ " " __TIME__

static const char *xsan_version_string = "1.0.0";
static const char *xsan_build_info = XSAN_VERSION_BUILD;

/**
 * 获取版本字符串
 */
const char *xsan_get_version(void)
{
    return xsan_version_string;
}

/**
 * 获取构建信息
 */
const char *xsan_get_build_info(void)
{
    return xsan_build_info;
}

/**
 * 获取版本号
 */
void xsan_get_version_numbers(int *major, int *minor, int *patch)
{
    if (major) *major = XSAN_VERSION_MAJOR;
    if (minor) *minor = XSAN_VERSION_MINOR;
    if (patch) *patch = XSAN_VERSION_PATCH;
}

/**
 * 打印版本信息
 */
void xsan_print_version(void)
{
    printf("XSAN Distributed Storage System\n");
    printf("Version: %s\n", xsan_version_string);
    printf("Built: %s\n", xsan_build_info);
    printf("Copyright (c) 2024 XSAN Project\n");
}
