#include "atx_controller.h"
#include "common.h"
#include "http_server.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "string.h"

static const char *TAG = "atx_controller";
static PowerState_t s_current_state = POWER_STATE_STANDBY;
static bool s_mainboard_power = false;
static QueueHandle_t s_event_queue = NULL;
static portMUX_TYPE s_state_mutex = portMUX_INITIALIZER_UNLOCKED;

// GPIO初始化
static void gpio_init_config(void) {
    gpio_config_t input_conf = {
        .pin_bit_mask = (1ULL << MAINBOARD_PWR_SW_IN_PIN) | (1ULL << MAINBOARD_PWR_LED_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&input_conf);

    gpio_config_t output_conf = {
        .pin_bit_mask = (1ULL << ATX_PS_ON_PIN) | (1ULL << MAINBOARD_PWR_SW_OUT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&output_conf);

    // 初始状态：待机
    gpio_set_level(ATX_PS_ON_PIN, 1);
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 1);
}

// 电源控制函数
static void send_power_on_signal(void) {
    ESP_LOGI(TAG, "发送开机信号");
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 0);
    vTaskDelay(POWER_ON_SIGNAL_MS / portTICK_PERIOD_MS);
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 1);
}

static void send_power_off_signal(void) {
    ESP_LOGI(TAG, "发送关机信号");
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 0);
    while (1) {
        if (!s_mainboard_power) {
            gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 1);
            break;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// 状态更新
static void update_mainboard_power_state(void) {
    bool new_state = !gpio_get_level(MAINBOARD_PWR_LED_PIN);
    portENTER_CRITICAL(&s_state_mutex);
    s_mainboard_power = new_state;
    portEXIT_CRITICAL(&s_state_mutex);
}

// 监听任务
static void monitor_task(void *arg) {
    bool last_switch_state = true;
    uint64_t last_print_time = 0;
    while (1) {

        update_mainboard_power_state();
        bool current_switch = gpio_get_level(MAINBOARD_PWR_SW_IN_PIN);
        uint64_t now = common_get_time_ms();

        // 开关防抖检测
        if (current_switch != last_switch_state) {
            vTaskDelay(DEBOUNCE_DELAY_MS / portTICK_PERIOD_MS);
            current_switch = gpio_get_level(MAINBOARD_PWR_SW_IN_PIN);
            if (current_switch == last_switch_state) continue;
            last_switch_state = current_switch;

            static uint64_t press_start_time = 0;
            if (!current_switch) {
                press_start_time = now;
            } else {
                uint64_t press_duration = now - press_start_time;
                if (press_duration < PWR_SW_HOLD_FOR_SHUTDOWN_MS && s_current_state == POWER_STATE_STANDBY) {
                    xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_SHORT_PRESS}, 0);
                }
            }
        }

        // 长按检测
        if (!current_switch && last_switch_state == false) {
            uint64_t press_duration = now - ((uint64_t)arg);
            if (press_duration >= PWR_SW_HOLD_FOR_SHUTDOWN_MS && s_current_state == POWER_STATE_RUNNING) {
                xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_LONG_HOLD}, 0);
                last_switch_state = true; // 避免重复触发
            }
        }

        // 定时打印状态
        if (now - last_print_time >= 1000) {
            last_print_time = now;
            const char *state_str[] = {"待机", "启动中", "运行中", "主板关机中", "电源关机中"};
            ESP_LOGI(TAG, "状态：%s | 主板：%s | 开关：%s",
                     state_str[s_current_state],
                     s_mainboard_power ? "开机" : "关机",
                     current_switch ? "释放" : "按下");
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// 控制任务
static void control_task(void *arg) {
    uint64_t shutdown_start_time = 0;
    AppEvent_t event = APP_EVENT_NONE;
    while (1) {
        if (xQueueReceive(s_event_queue, &event, 0) == pdPASS) {
            switch (event) {
                case APP_EVENT_PWR_SW_SHORT_PRESS:
                    if (s_current_state == POWER_STATE_STANDBY) {
                        s_current_state = POWER_STATE_STARTING;
                    }
                    break;
                case APP_EVENT_PWR_SW_LONG_HOLD:
                    if (s_current_state == POWER_STATE_RUNNING) {
                        s_current_state = POWER_STATE_SHUTTING_DOWN_MAINBOARD;
                        shutdown_start_time = common_get_time_ms();
                    }
                    break;
                default:
                    break;
            }
            event = APP_EVENT_NONE;
        }


        switch (s_current_state) {
            case POWER_STATE_STANDBY:
                gpio_set_level(ATX_PS_ON_PIN, 1);
                break;
            case POWER_STATE_STARTING:
                gpio_set_level(ATX_PS_ON_PIN, 0);
                vTaskDelay(POWER_ON_DELAY_MS / portTICK_PERIOD_MS);
                send_power_on_signal();
                update_mainboard_power_state();
                if (s_mainboard_power) {
                    s_current_state = POWER_STATE_RUNNING;
                } else {
                    s_current_state = POWER_STATE_STANDBY;
                }
                break;
            case POWER_STATE_RUNNING:
                gpio_set_level(ATX_PS_ON_PIN, 0);
                update_mainboard_power_state();
                if (!s_mainboard_power) {
                    s_current_state = POWER_STATE_SHUTTING_DOWN_POWER;
                    shutdown_start_time = common_get_time_ms();
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
                break;
            case POWER_STATE_SHUTTING_DOWN_MAINBOARD:
                send_power_off_signal();
                s_current_state = POWER_STATE_SHUTTING_DOWN_POWER;
                shutdown_start_time = common_get_time_ms();
                break;
            case POWER_STATE_SHUTTING_DOWN_POWER:
                if (common_get_time_ms() - shutdown_start_time >= POWER_OFF_DELAY_MS) {
                    gpio_set_level(ATX_PS_ON_PIN, 1);
                    s_current_state = POWER_STATE_STANDBY;
                }
                break;
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

static bool parse_key_param(httpd_req_t *req, char *key_buf, size_t buf_len) {
    // 1. 获取 URL 中的查询字符串（如 "key=your_control_key"）
    char query_str[64];  // 存储查询字符串的缓冲区（根据实际需求调整大小）
    if (httpd_req_get_url_query_str(req, query_str, sizeof(query_str)) != ESP_OK) {
        ESP_LOGE(TAG, "获取查询字符串失败");
        return false;
    }

    // 2. 解析查询字符串中的 "key" 参数（格式：key=xxx）
    char *key_val = strstr(query_str, "key=");
    if (!key_val) {
        ESP_LOGE(TAG, "未找到 key 参数");
        return false;
    }
    key_val += strlen("key=");  // 跳过 "key="，指向参数值

    // 3. 复制参数值到缓冲区（确保不溢出）
    strlcpy(key_buf, key_val, buf_len);
    return true;
}


// 远程控制处理
static esp_err_t http_handler_control_start(httpd_req_t *req) {
    char key[32];
    if (!parse_key_param(req, key, sizeof(key)) || strcmp(key, CONTROL_SECRET_KEY) != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        const char *resp = "{\"code\":-1,\"msg\":\"密钥错误或缺失\"}";
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    if (s_current_state == POWER_STATE_STANDBY) {
        xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_SHORT_PRESS}, 100 / portTICK_PERIOD_MS);
        httpd_resp_send(req, "{\"status\":\"success\",\"msg\":\"开机命令已发送\"}", -1);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "当前状态不支持开机");
    }
    return ESP_OK;
}

static esp_err_t http_handler_control_shutdown(httpd_req_t *req) {
    char key[32];
    if (!parse_key_param(req, key, sizeof(key)) || strcmp(key, CONTROL_SECRET_KEY) != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        const char *resp = "{\"code\":-1,\"msg\":\"密钥错误或缺失\"}";
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }

    if (s_current_state == POWER_STATE_RUNNING) {
        xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_LONG_HOLD}, 100 / portTICK_PERIOD_MS);
        httpd_resp_send(req, "{\"status\":\"success\",\"msg\":\"关机命令已发送\"}", -1);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "当前状态不支持关机");
    }
    return ESP_OK;
}

// ATX模块路由
static const HttpRoute_t s_atx_routes[] = {
    {.uri = "/control/start", .method = HTTP_GET, .handler = http_handler_control_start, .user_ctx = NULL},
    {.uri = "/control/shutdown", .method = HTTP_GET, .handler = http_handler_control_shutdown, .user_ctx = NULL},
};

// ATX状态回调（给Web模块）
static char* atx_state_callback(void) {
    char *state = (char *)malloc(256);
    const char *state_str[] = {"待机", "启动中", "运行中", "主板关机中", "电源关机中"};
    portENTER_CRITICAL(&s_state_mutex);
    snprintf(state, 256, "  \"电源状态\": \"%s\",\n  \"主板电源\": \"%s\",\n  \"PS_ON引脚\": \"%s\"",
             state_str[s_current_state],
             s_mainboard_power ? "开机" : "关机",
             gpio_get_level(ATX_PS_ON_PIN) ? "高电平(待机)" : "低电平(运行)");
    portEXIT_CRITICAL(&s_state_mutex);
    return state;
}

esp_err_t atx_controller_init(void) {
    gpio_init_config();
    s_event_queue = xQueueCreate(5, sizeof(AppEvent_t));
    if (!s_event_queue) return ESP_ERR_NO_MEM;

    // 注册状态回调
    esp_err_t ret = http_server_register_state_callback(atx_state_callback);
    if (ret != ESP_OK) return ret;

    // 创建任务
    xTaskCreate(monitor_task, "atx_monitor", 2048, NULL, 5, NULL);
    xTaskCreate(control_task, "atx_control", 4096, NULL, 5, NULL);

    return ESP_OK;
}

PowerState_t atx_controller_get_state(void) {
    portENTER_CRITICAL(&s_state_mutex);
    PowerState_t state = s_current_state;
    portEXIT_CRITICAL(&s_state_mutex);
    return state;
}

bool atx_controller_get_mainboard_power(void) {
    portENTER_CRITICAL(&s_state_mutex);
    bool power = s_mainboard_power;
    portEXIT_CRITICAL(&s_state_mutex);
    return power;
}

esp_err_t atx_controller_register_web_routes(void) {
    return http_server_register_routes(s_atx_routes, sizeof(s_atx_routes)/sizeof(HttpRoute_t));
}

QueueHandle_t atx_controller_get_event_queue(void) {
    return s_event_queue;
}