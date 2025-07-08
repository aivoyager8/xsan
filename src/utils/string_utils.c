/**
 * XSAN 字符串工具模块实现
 * 
 * 提供安全的字符串操作函数
 */

#include "xsan_string_utils.h"
#include "xsan_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>

/**
 * 安全的字符串复制
 */
size_t xsan_strcpy_safe(char *dest, const char *src, size_t dest_size)
{
    if (!dest || !src || dest_size == 0) {
        return 0;
    }
    
    size_t src_len = strlen(src);
    size_t copy_len = (src_len < dest_size - 1) ? src_len : dest_size - 1;
    
    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
    
    return copy_len;
}

/**
 * 安全的字符串连接
 */
size_t xsan_strcat_safe(char *dest, const char *src, size_t dest_size)
{
    if (!dest || !src || dest_size == 0) {
        return 0;
    }
    
    size_t dest_len = strnlen(dest, dest_size);
    if (dest_len >= dest_size) {
        return dest_len;
    }
    
    size_t remaining = dest_size - dest_len;
    size_t src_len = strlen(src);
    size_t copy_len = (src_len < remaining - 1) ? src_len : remaining - 1;
    
    memcpy(dest + dest_len, src, copy_len);
    dest[dest_len + copy_len] = '\0';
    
    return dest_len + copy_len;
}

/**
 * 安全的格式化字符串
 */
int xsan_snprintf_safe(char *dest, size_t dest_size, const char *format, ...)
{
    if (!dest || !format || dest_size == 0) {
        return -1;
    }
    
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(dest, dest_size, format, args);
    va_end(args);
    
    if (ret >= 0 && (size_t)ret < dest_size) {
        return ret;
    }
    
    dest[dest_size - 1] = '\0';
    return (int)dest_size - 1;
}

/**
 * 字符串分割
 */
size_t xsan_strsplit(const char *str, const char *delim, char **tokens, size_t max_tokens)
{
    if (!str || !delim || !tokens || max_tokens == 0) {
        return 0;
    }
    
    size_t token_count = 0;
    char *str_copy = xsan_strdup(str);
    if (!str_copy) {
        return 0;
    }
    
    char *token = strtok(str_copy, delim);
    while (token && token_count < max_tokens) {
        tokens[token_count] = xsan_strdup(token);
        if (!tokens[token_count]) {
            /* 清理已分配的内存 */
            for (size_t i = 0; i < token_count; i++) {
                xsan_free(tokens[i]);
            }
            xsan_free(str_copy);
            return 0;
        }
        token_count++;
        token = strtok(NULL, delim);
    }
    
    xsan_free(str_copy);
    return token_count;
}

/**
 * 去除字符串前后空白字符
 */
char *xsan_strtrim(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    /* 跳过前面的空白字符 */
    while (isspace((unsigned char)*str)) {
        str++;
    }
    
    if (*str == '\0') {
        return xsan_strdup("");
    }
    
    /* 找到最后一个非空白字符 */
    const char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    
    size_t len = end - str + 1;
    char *result = xsan_malloc(len + 1);
    if (!result) {
        return NULL;
    }
    
    memcpy(result, str, len);
    result[len] = '\0';
    
    return result;
}

/**
 * 去除字符串前面的空白字符
 */
char *xsan_strtrim_left(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    while (isspace((unsigned char)*str)) {
        str++;
    }
    
    return xsan_strdup(str);
}

/**
 * 去除字符串后面的空白字符
 */
char *xsan_strtrim_right(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str);
    if (len == 0) {
        return xsan_strdup("");
    }
    
    const char *end = str + len - 1;
    while (end >= str && isspace((unsigned char)*end)) {
        end--;
    }
    
    size_t new_len = end - str + 1;
    char *result = xsan_malloc(new_len + 1);
    if (!result) {
        return NULL;
    }
    
    memcpy(result, str, new_len);
    result[new_len] = '\0';
    
    return result;
}

/**
 * 字符串转大写
 */
char *xsan_strupper(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str);
    char *result = xsan_malloc(len + 1);
    if (!result) {
        return NULL;
    }
    
    for (size_t i = 0; i < len; i++) {
        result[i] = toupper((unsigned char)str[i]);
    }
    result[len] = '\0';
    
    return result;
}

/**
 * 字符串转小写
 */
char *xsan_strlower(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str);
    char *result = xsan_malloc(len + 1);
    if (!result) {
        return NULL;
    }
    
    for (size_t i = 0; i < len; i++) {
        result[i] = tolower((unsigned char)str[i]);
    }
    result[len] = '\0';
    
    return result;
}

/**
 * 不区分大小写的字符串比较
 */
int xsan_strcasecmp(const char *s1, const char *s2)
{
    if (!s1 || !s2) {
        return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    }
    
    while (*s1 && *s2) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/**
 * 不区分大小写的字符串比较（指定长度）
 */
int xsan_strncasecmp(const char *s1, const char *s2, size_t n)
{
    if (!s1 || !s2 || n == 0) {
        return (s1 == s2) ? 0 : (s1 ? 1 : -1);
    }
    
    for (size_t i = 0; i < n && *s1 && *s2; i++) {
        int c1 = tolower((unsigned char)*s1);
        int c2 = tolower((unsigned char)*s2);
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    
    return 0;
}

/**
 * 检查字符串是否以指定前缀开始
 */
bool xsan_str_starts_with(const char *str, const char *prefix)
{
    if (!str || !prefix) {
        return false;
    }
    
    size_t str_len = strlen(str);
    size_t prefix_len = strlen(prefix);
    
    if (prefix_len > str_len) {
        return false;
    }
    
    return memcmp(str, prefix, prefix_len) == 0;
}

/**
 * 检查字符串是否以指定后缀结束
 */
bool xsan_str_ends_with(const char *str, const char *suffix)
{
    if (!str || !suffix) {
        return false;
    }
    
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    
    if (suffix_len > str_len) {
        return false;
    }
    
    return memcmp(str + str_len - suffix_len, suffix, suffix_len) == 0;
}

/**
 * 检查字符串是否包含指定子串
 */
bool xsan_str_contains(const char *str, const char *substr)
{
    if (!str || !substr) {
        return false;
    }
    
    return strstr(str, substr) != NULL;
}

/**
 * 替换字符串中的子串
 */
char *xsan_str_replace(const char *str, const char *old_substr, const char *new_substr)
{
    if (!str || !old_substr || !new_substr) {
        return NULL;
    }
    
    size_t str_len = strlen(str);
    size_t old_len = strlen(old_substr);
    size_t new_len = strlen(new_substr);
    
    if (old_len == 0) {
        return xsan_strdup(str);
    }
    
    /* 计算需要替换的次数 */
    size_t count = 0;
    const char *pos = str;
    while ((pos = strstr(pos, old_substr)) != NULL) {
        count++;
        pos += old_len;
    }
    
    if (count == 0) {
        return xsan_strdup(str);
    }
    
    /* 计算新字符串长度 */
    size_t new_str_len = str_len + count * (new_len - old_len);
    char *result = xsan_malloc(new_str_len + 1);
    if (!result) {
        return NULL;
    }
    
    /* 执行替换 */
    char *dest = result;
    const char *src = str;
    while ((pos = strstr(src, old_substr)) != NULL) {
        size_t prefix_len = pos - src;
        memcpy(dest, src, prefix_len);
        dest += prefix_len;
        
        memcpy(dest, new_substr, new_len);
        dest += new_len;
        
        src = pos + old_len;
    }
    
    strcpy(dest, src);
    
    return result;
}

/**
 * 字符串反转
 */
char *xsan_str_reverse(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str);
    char *result = xsan_malloc(len + 1);
    if (!result) {
        return NULL;
    }
    
    for (size_t i = 0; i < len; i++) {
        result[i] = str[len - 1 - i];
    }
    result[len] = '\0';
    
    return result;
}

/**
 * 计算字符串的哈希值（DJB2算法）
 */
uint32_t xsan_str_hash(const char *str)
{
    if (!str) {
        return 0;
    }
    
    uint32_t hash = 5381;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    
    return hash;
}

/**
 * 字符串到整数的转换
 */
bool xsan_str_to_int(const char *str, int *value)
{
    if (!str || !value) {
        return false;
    }
    
    char *endptr;
    errno = 0;
    long result = strtol(str, &endptr, 10);
    
    if (errno == ERANGE || *endptr != '\0' || endptr == str) {
        return false;
    }
    
    if (result < INT_MIN || result > INT_MAX) {
        return false;
    }
    
    *value = (int)result;
    return true;
}

/**
 * 字符串到长整数的转换
 */
bool xsan_str_to_long(const char *str, long *value)
{
    if (!str || !value) {
        return false;
    }
    
    char *endptr;
    errno = 0;
    long result = strtol(str, &endptr, 10);
    
    if (errno == ERANGE || *endptr != '\0' || endptr == str) {
        return false;
    }
    
    *value = result;
    return true;
}

/**
 * 字符串到双精度浮点数的转换
 */
bool xsan_str_to_double(const char *str, double *value)
{
    if (!str || !value) {
        return false;
    }
    
    char *endptr;
    errno = 0;
    double result = strtod(str, &endptr);
    
    if (errno == ERANGE || *endptr != '\0' || endptr == str) {
        return false;
    }
    
    *value = result;
    return true;
}

/**
 * 字符串到布尔值的转换
 */
bool xsan_str_to_bool(const char *str, bool *value)
{
    if (!str || !value) {
        return false;
    }
    
    char *lower_str = xsan_strlower(str);
    if (!lower_str) {
        return false;
    }
    
    bool result = false;
    if (strcmp(lower_str, "true") == 0 || strcmp(lower_str, "yes") == 0 || 
        strcmp(lower_str, "on") == 0 || strcmp(lower_str, "1") == 0) {
        *value = true;
        result = true;
    } else if (strcmp(lower_str, "false") == 0 || strcmp(lower_str, "no") == 0 || 
               strcmp(lower_str, "off") == 0 || strcmp(lower_str, "0") == 0) {
        *value = false;
        result = true;
    }
    
    xsan_free(lower_str);
    return result;
}

/**
 * 将字节数转换为人类可读的字符串
 */
bool xsan_bytes_to_human_readable(uint64_t bytes, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 16) {
        return false;
    }
    
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    const size_t num_units = sizeof(units) / sizeof(units[0]);
    
    double size = (double)bytes;
    size_t unit_index = 0;
    
    while (size >= 1024.0 && unit_index < num_units - 1) {
        size /= 1024.0;
        unit_index++;
    }
    
    if (unit_index == 0) {
        snprintf(buffer, buffer_size, "%lu %s", bytes, units[unit_index]);
    } else {
        snprintf(buffer, buffer_size, "%.2f %s", size, units[unit_index]);
    }
    
    return true;
}

/**
 * 将时间间隔转换为人类可读的字符串
 */
bool xsan_duration_to_human_readable(uint64_t seconds, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size < 32) {
        return false;
    }
    
    if (seconds < 60) {
        snprintf(buffer, buffer_size, "%lu秒", seconds);
    } else if (seconds < 3600) {
        snprintf(buffer, buffer_size, "%lu分%lu秒", seconds / 60, seconds % 60);
    } else if (seconds < 86400) {
        uint64_t hours = seconds / 3600;
        uint64_t minutes = (seconds % 3600) / 60;
        snprintf(buffer, buffer_size, "%lu小时%lu分", hours, minutes);
    } else {
        uint64_t days = seconds / 86400;
        uint64_t hours = (seconds % 86400) / 3600;
        snprintf(buffer, buffer_size, "%lu天%lu小时", days, hours);
    }
    
    return true;
}

/**
 * 解析配置文件中的键值对
 */
bool xsan_parse_config_line(const char *line, char *key, char *value, 
                           size_t key_size, size_t value_size)
{
    if (!line || !key || !value || key_size == 0 || value_size == 0) {
        return false;
    }
    
    /* 跳过前面的空白字符 */
    while (isspace((unsigned char)*line)) {
        line++;
    }
    
    /* 跳过注释行和空行 */
    if (*line == '#' || *line == '\0') {
        return false;
    }
    
    /* 查找等号 */
    const char *equal_pos = strchr(line, '=');
    if (!equal_pos) {
        return false;
    }
    
    /* 提取键 */
    const char *key_start = line;
    const char *key_end = equal_pos - 1;
    while (key_end >= key_start && isspace((unsigned char)*key_end)) {
        key_end--;
    }
    
    size_t key_len = key_end - key_start + 1;
    if (key_len >= key_size) {
        return false;
    }
    
    memcpy(key, key_start, key_len);
    key[key_len] = '\0';
    
    /* 提取值 */
    const char *value_start = equal_pos + 1;
    while (isspace((unsigned char)*value_start)) {
        value_start++;
    }
    
    const char *value_end = value_start + strlen(value_start) - 1;
    while (value_end >= value_start && isspace((unsigned char)*value_end)) {
        value_end--;
    }
    
    size_t value_len = value_end - value_start + 1;
    if (value_len >= value_size) {
        return false;
    }
    
    memcpy(value, value_start, value_len);
    value[value_len] = '\0';
    
    return true;
}

/**
 * 转义字符串中的特殊字符
 */
char *xsan_str_escape(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str);
    size_t escaped_len = 0;
    
    /* 计算转义后的长度 */
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '\n':
            case '\r':
            case '\t':
            case '\\':
            case '"':
            case '\'':
                escaped_len += 2;
                break;
            default:
                escaped_len++;
                break;
        }
    }
    
    char *result = xsan_malloc(escaped_len + 1);
    if (!result) {
        return NULL;
    }
    
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '\n':
                result[j++] = '\\';
                result[j++] = 'n';
                break;
            case '\r':
                result[j++] = '\\';
                result[j++] = 'r';
                break;
            case '\t':
                result[j++] = '\\';
                result[j++] = 't';
                break;
            case '\\':
                result[j++] = '\\';
                result[j++] = '\\';
                break;
            case '"':
                result[j++] = '\\';
                result[j++] = '"';
                break;
            case '\'':
                result[j++] = '\\';
                result[j++] = '\'';
                break;
            default:
                result[j++] = str[i];
                break;
        }
    }
    
    result[j] = '\0';
    return result;
}

/**
 * 反转义字符串中的特殊字符
 */
char *xsan_str_unescape(const char *str)
{
    if (!str) {
        return NULL;
    }
    
    size_t len = strlen(str);
    char *result = xsan_malloc(len + 1);
    if (!result) {
        return NULL;
    }
    
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\\' && i + 1 < len) {
            switch (str[i + 1]) {
                case 'n':
                    result[j++] = '\n';
                    i++;
                    break;
                case 'r':
                    result[j++] = '\r';
                    i++;
                    break;
                case 't':
                    result[j++] = '\t';
                    i++;
                    break;
                case '\\':
                    result[j++] = '\\';
                    i++;
                    break;
                case '"':
                    result[j++] = '"';
                    i++;
                    break;
                case '\'':
                    result[j++] = '\'';
                    i++;
                    break;
                default:
                    result[j++] = str[i];
                    break;
            }
        } else {
            result[j++] = str[i];
        }
    }
    
    result[j] = '\0';
    return result;
}
