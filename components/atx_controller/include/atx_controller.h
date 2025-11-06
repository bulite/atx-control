#ifndef ATX_CONTROLLER_H
#define ATX_CONTROLLER_H

#include <esp_err.h>
#include "http_server.h"
#include "common.h"

// 引脚配置（用户需修改）
#define ATX_PS_ON_PIN          3
#define MAINBOARD_PWR_SW_IN_PIN 0
#define MAINBOARD_PWR_SW_OUT_PIN 1
#define MAINBOARD_PWR_LED_PIN  2

// 时序配置
#define DEBOUNCE_DELAY_MS      50
#define POWER_ON_DELAY_MS      1000
#define POWER_ON_SIGNAL_MS     500
#define PWR_SW_HOLD_FOR_SHUTDOWN_MS 2000
#define POWER_OFF_DELAY_MS     4000
#define CONTROL_SECRET_KEY    "your_control_key"

// 电源状态
typedef enum {
    POWER_STATE_STANDBY,
    POWER_STATE_STARTING,
    POWER_STATE_RUNNING,
    POWER_STATE_SHUTTING_DOWN_MAINBOARD,
    POWER_STATE_SHUTTING_DOWN_POWER
} PowerState_t;

esp_err_t atx_controller_init(void);
PowerState_t atx_controller_get_state(void);
bool atx_controller_get_mainboard_power(void);
esp_err_t atx_controller_register_web_routes(void);
QueueHandle_t atx_controller_get_event_queue(void);

#endif // ATX_CONTROLLER_H