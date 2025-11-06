#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// 全局类型定义
typedef enum {
    APP_EVENT_NONE,
    APP_EVENT_PWR_SW_SHORT_PRESS,
    APP_EVENT_PWR_SW_LONG_HOLD
} AppEvent_t;

// 日志缓冲区配置
#define LOG_BUFFER_SIZE     16384
#define LOG_MUTEX           portMUX_INITIALIZER_UNLOCKED

// 工具函数声明
uint64_t common_get_time_ms(void);
void common_log_buffer_write(const char *data, size_t len);
char* common_log_buffer_read(size_t *read_len);
bool common_base64_decode(const char *in, size_t in_len, char *out, size_t out_len);
bool common_md5_verify(const uint8_t *data, size_t len, const char *expected_md5);
esp_err_t common_log_init(void);

#endif // COMMON_H