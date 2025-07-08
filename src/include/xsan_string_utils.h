/**
 * XSAN 字符串工具模块
 * 
 * 提供安全的字符串操作函数
 */

#ifndef XSAN_STRING_UTILS_H
#define XSAN_STRING_UTILS_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 安全的字符串复制
 * 
 * @param dest 目标缓冲区
 * @param src 源字符串
 * @param dest_size 目标缓冲区大小
 * @return 复制的字符数
 */
size_t xsan_strcpy_safe(char *dest, const char *src, size_t dest_size);

/**
 * 安全的字符串连接
 * 
 * @param dest 目标缓冲区
 * @param src 源字符串
 * @param dest_size 目标缓冲区大小
 * @return 连接后的字符串长度
 */
size_t xsan_strcat_safe(char *dest, const char *src, size_t dest_size);

/**
 * 安全的格式化字符串
 * 
 * @param dest 目标缓冲区
 * @param dest_size 目标缓冲区大小
 * @param format 格式字符串
 * @param ... 参数
 * @return 格式化后的字符串长度
 */
int xsan_snprintf_safe(char *dest, size_t dest_size, const char *format, ...);

/**
 * 字符串分割
 * 
 * @param str 要分割的字符串
 * @param delim 分隔符
 * @param tokens 输出的字符串数组
 * @param max_tokens 最大token数量
 * @return 实际分割的token数量
 */
size_t xsan_strsplit(const char *str, const char *delim, char **tokens, size_t max_tokens);

/**
 * 去除字符串前后空白字符
 * 
 * @param str 字符串
 * @return 去除空白字符后的字符串（需要释放内存）
 */
char *xsan_strtrim(const char *str);

/**
 * 去除字符串前面的空白字符
 * 
 * @param str 字符串
 * @return 去除前面空白字符后的字符串（需要释放内存）
 */
char *xsan_strtrim_left(const char *str);

/**
 * 去除字符串后面的空白字符
 * 
 * @param str 字符串
 * @return 去除后面空白字符后的字符串（需要释放内存）
 */
char *xsan_strtrim_right(const char *str);

/**
 * 字符串转大写
 * 
 * @param str 字符串
 * @return 大写字符串（需要释放内存）
 */
char *xsan_strupper(const char *str);

/**
 * 字符串转小写
 * 
 * @param str 字符串
 * @return 小写字符串（需要释放内存）
 */
char *xsan_strlower(const char *str);

/**
 * 不区分大小写的字符串比较
 * 
 * @param s1 字符串1
 * @param s2 字符串2
 * @return 比较结果
 */
int xsan_strcasecmp(const char *s1, const char *s2);

/**
 * 不区分大小写的字符串比较（指定长度）
 * 
 * @param s1 字符串1
 * @param s2 字符串2
 * @param n 比较长度
 * @return 比较结果
 */
int xsan_strncasecmp(const char *s1, const char *s2, size_t n);

/**
 * 检查字符串是否以指定前缀开始
 * 
 * @param str 字符串
 * @param prefix 前缀
 * @return 是否以prefix开始
 */
bool xsan_str_starts_with(const char *str, const char *prefix);

/**
 * 检查字符串是否以指定后缀结束
 * 
 * @param str 字符串
 * @param suffix 后缀
 * @return 是否以suffix结束
 */
bool xsan_str_ends_with(const char *str, const char *suffix);

/**
 * 检查字符串是否包含指定子串
 * 
 * @param str 字符串
 * @param substr 子串
 * @return 是否包含substr
 */
bool xsan_str_contains(const char *str, const char *substr);

/**
 * 替换字符串中的子串
 * 
 * @param str 原字符串
 * @param old_substr 要替换的子串
 * @param new_substr 新子串
 * @return 替换后的字符串（需要释放内存）
 */
char *xsan_str_replace(const char *str, const char *old_substr, const char *new_substr);

/**
 * 字符串反转
 * 
 * @param str 字符串
 * @return 反转后的字符串（需要释放内存）
 */
char *xsan_str_reverse(const char *str);

/**
 * 计算字符串的哈希值
 * 
 * @param str 字符串
 * @return 哈希值
 */
uint32_t xsan_str_hash(const char *str);

/**
 * 字符串到整数的转换
 * 
 * @param str 字符串
 * @param value 输出的整数值
 * @return 是否转换成功
 */
bool xsan_str_to_int(const char *str, int *value);

/**
 * 字符串到长整数的转换
 * 
 * @param str 字符串
 * @param value 输出的长整数值
 * @return 是否转换成功
 */
bool xsan_str_to_long(const char *str, long *value);

/**
 * 字符串到双精度浮点数的转换
 * 
 * @param str 字符串
 * @param value 输出的双精度浮点数值
 * @return 是否转换成功
 */
bool xsan_str_to_double(const char *str, double *value);

/**
 * 字符串到布尔值的转换
 * 
 * @param str 字符串
 * @param value 输出的布尔值
 * @return 是否转换成功
 */
bool xsan_str_to_bool(const char *str, bool *value);

/**
 * 将字节数转换为人类可读的字符串
 * 
 * @param bytes 字节数
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 是否转换成功
 */
bool xsan_bytes_to_human_readable(uint64_t bytes, char *buffer, size_t buffer_size);

/**
 * 将时间间隔转换为人类可读的字符串
 * 
 * @param seconds 秒数
 * @param buffer 输出缓冲区
 * @param buffer_size 缓冲区大小
 * @return 是否转换成功
 */
bool xsan_duration_to_human_readable(uint64_t seconds, char *buffer, size_t buffer_size);

/**
 * 解析配置文件中的键值对
 * 
 * @param line 配置行
 * @param key 输出的键
 * @param value 输出的值
 * @param key_size 键缓冲区大小
 * @param value_size 值缓冲区大小
 * @return 是否解析成功
 */
bool xsan_parse_config_line(const char *line, char *key, char *value, 
                           size_t key_size, size_t value_size);

/**
 * 转义字符串中的特殊字符
 * 
 * @param str 原字符串
 * @return 转义后的字符串（需要释放内存）
 */
char *xsan_str_escape(const char *str);

/**
 * 反转义字符串中的特殊字符
 * 
 * @param str 转义后的字符串
 * @return 反转义后的字符串（需要释放内存）
 */
char *xsan_str_unescape(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* XSAN_STRING_UTILS_H */
