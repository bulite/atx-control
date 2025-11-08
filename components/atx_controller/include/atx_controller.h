#ifndef ATX_CONTROLLER_H
#define ATX_CONTROLLER_H

#include <esp_err.h>
#include "esp_http_server.h"

// 引脚配置（用户需修改）
#define ATX_PS_ON_PIN            3
#define MAINBOARD_PWR_SW_IN_PIN  0
#define MAINBOARD_PWR_SW_OUT_PIN 1
#define MAINBOARD_PWR_LED_PIN    2

// 时序配置
#define DEBOUNCE_DELAY_MS      20
#define POWER_ON_DELAY_MS      1000
#define POWER_ON_SIGNAL_MS     500
#define PWR_SW_HOLD_FOR_SHUTDOWN_MS 2000
#define POWER_OFF_DELAY_MS     4000
#define POWER_ON_TIMEOUT_MS (30000)      // 开机超时30秒
#define POWER_OFF_TIMEOUT_MS (30000)     // 关机超时30秒

// 应用事件类型
typedef enum {
    APP_EVENT_NONE,
    APP_EVENT_PWR_SW_SHORT_PRESS,
    APP_EVENT_PWR_SW_LONG_HOLD,
} AppEvent_t;

esp_err_t atx_controller_init(void);
#endif // ATX_CONTROLLER_H