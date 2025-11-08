#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

// 日志缓冲区配置
#define LOG_BUFFER_SIZE     16384

// 状态回调函数类型（用于Web页面获取各模块状态）
typedef char* (*stateCallback_t)(void);

// 工具函数声明
uint64_t common_get_time_ms(void);
bool common_base64_decode(const char *in, size_t in_len, char *out, size_t out_len);
bool common_md5_verify(const uint8_t *data, size_t len, const char *expected_md5);
esp_err_t common_register_state_callback(stateCallback_t callback);
esp_err_t common_init(void);
#endif // COMMON_H