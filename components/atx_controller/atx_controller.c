#include "atx_controller.h"
#include "common.h"
#include "http_server.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "string.h"

// 电源状态
typedef enum
{
    POWER_STATE_STANDBY,
    POWER_STATE_STARTING,
    POWER_STATE_RUNNING,
    POWER_STATE_SHUTTING_DOWN_MAINBOARD,
    POWER_STATE_SHUTTING_DOWN_POWER
} PowerState_t;

static const char *TAG = "atx_controller";
static PowerState_t s_current_state = POWER_STATE_STANDBY;
static bool s_mainboard_power = false;
static QueueHandle_t s_event_queue = NULL;
static portMUX_TYPE s_state_mutex = portMUX_INITIALIZER_UNLOCKED;
static bool ps_on_satus = false;

// GPIO初始化
static void gpio_init_config(void)
{
    gpio_config_t input_conf2 = {
        .pin_bit_mask = (1ULL << MAINBOARD_PWR_SW_IN_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&input_conf2);

    gpio_config_t input_conf1 = {
        .pin_bit_mask = (1ULL << MAINBOARD_PWR_LED_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&input_conf1);

    gpio_config_t output_conf = {
        .pin_bit_mask = (1ULL << ATX_PS_ON_PIN) | (1ULL << MAINBOARD_PWR_SW_OUT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&output_conf);

    // 初始状态：待机
    gpio_set_level(ATX_PS_ON_PIN, 1);
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 1);
}

// 电源控制函数
static void send_power_on_signal(void)
{
    ESP_LOGI(TAG, "发送开机信号");
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 0);
    vTaskDelay(POWER_ON_SIGNAL_MS / portTICK_PERIOD_MS);
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 1);
}

// 状态更新
static void update_mainboard_power_state(void)
{
    bool new_state = gpio_get_level(MAINBOARD_PWR_LED_PIN);
    portENTER_CRITICAL(&s_state_mutex);
    s_mainboard_power = new_state;
    portEXIT_CRITICAL(&s_state_mutex);
}

// 修复send_power_off_signal，添加超时机制
static void send_power_off_signal(void)
{
    ESP_LOGI(TAG, "发送关机信号");
    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 0);

    uint64_t start_time = common_get_time_ms();
    while (s_mainboard_power &&
           (common_get_time_ms() - start_time < POWER_OFF_TIMEOUT_MS))
    {
        update_mainboard_power_state();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    gpio_set_level(MAINBOARD_PWR_SW_OUT_PIN, 1);

    if (s_mainboard_power)
    {
        ESP_LOGW(TAG, "关机信号发送超时");
    }
}

static void monitor_task(void *arg)
{
    bool stable_switch_state = true;
    uint64_t last_print_time = 0;
    static uint64_t press_start_time = 0;
    uint32_t debounce_counter = 0;

    while (1)
    {
        update_mainboard_power_state();
        bool current_switch = gpio_get_level(MAINBOARD_PWR_SW_IN_PIN);

        // 改进的防抖逻辑
        if (current_switch != stable_switch_state)
        {
            debounce_counter++;
            if (debounce_counter >= 5)
            { // 100ms稳定期
                stable_switch_state = current_switch;

                if (!stable_switch_state)
                {
                    // 按下
                    press_start_time = common_get_time_ms();
                    ESP_LOGI(TAG, "电源开关按下");
                }
                else
                {
                    // 释放
                    uint64_t press_duration = common_get_time_ms() - press_start_time;
                    ESP_LOGI(TAG, "电源开关释放，持续时间: %llums", press_duration);

                    if (press_duration < PWR_SW_HOLD_FOR_SHUTDOWN_MS)
                    {
                        // 短按
                        if (s_current_state == POWER_STATE_STANDBY)
                        {
                            xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_SHORT_PRESS}, 0);
                        }
                    }
                    else
                    {
                        // 长按
                        if (s_current_state == POWER_STATE_RUNNING)
                        {
                            xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_LONG_HOLD}, 0);
                        }
                    }
                }
                debounce_counter = 0;
            }
        }
        else
        {
            debounce_counter = 0;
        }

        // 状态打印
        uint64_t now = common_get_time_ms();
        if (now - last_print_time >= 5000)
        { // 5秒打印一次
            last_print_time = now;
            const char *state_str[] = {"待机", "启动中", "运行中", "主板关机中", "电源关机中"};
            ESP_LOGI(TAG, "状态：%s | 主板：%s | 开关：%s",
                     state_str[s_current_state],
                     s_mainboard_power ? "开机" : "关机",
                     stable_switch_state ? "释放" : "按下");
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// 控制任务
static void control_task(void *arg)
{
    uint64_t shutdown_start_time = 0;
    AppEvent_t event = APP_EVENT_NONE;
    while (1)
    {
        if (xQueueReceive(s_event_queue, &event, 0) == pdPASS)
        {
            switch (event)
            {
            case APP_EVENT_PWR_SW_SHORT_PRESS:
                if (s_current_state == POWER_STATE_STANDBY)
                {
                    s_current_state = POWER_STATE_STARTING;
                }
                break;
            case APP_EVENT_PWR_SW_LONG_HOLD:
                if (s_current_state == POWER_STATE_RUNNING)
                {
                    s_current_state = POWER_STATE_SHUTTING_DOWN_MAINBOARD;
                    shutdown_start_time = common_get_time_ms();
                }
                break;
            default:
                break;
            }
            event = APP_EVENT_NONE;
        }

        switch (s_current_state)
        {
        case POWER_STATE_STANDBY:
            gpio_set_level(ATX_PS_ON_PIN, 1);
            ps_on_satus = 1;
            break;
        // 在control_task中添加超时处理
        case POWER_STATE_STARTING:
        {
            uint64_t start_time = common_get_time_ms();
            gpio_set_level(ATX_PS_ON_PIN, 0);
            ps_on_satus = 0;
            vTaskDelay(POWER_ON_DELAY_MS / portTICK_PERIOD_MS);
            send_power_on_signal();

            // 等待主板启动，超时返回待机
            while (common_get_time_ms() - start_time < POWER_ON_TIMEOUT_MS)
            {
                update_mainboard_power_state();
                if (s_mainboard_power)
                {
                    s_current_state = POWER_STATE_RUNNING;
                    ESP_LOGI(TAG, "主板启动成功");
                    break;
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }

            if (!s_mainboard_power)
            {
                s_current_state = POWER_STATE_STANDBY;
                gpio_set_level(ATX_PS_ON_PIN, 1); // 关闭ATX电源
                ps_on_satus = 1;
                ESP_LOGW(TAG, "主板启动超时，返回待机状态");
            }
            break;
        }
        case POWER_STATE_RUNNING:
            gpio_set_level(ATX_PS_ON_PIN, 0);
            ps_on_satus = 0;
            update_mainboard_power_state();
            if (!s_mainboard_power)
            {
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
            if (common_get_time_ms() - shutdown_start_time >= POWER_OFF_DELAY_MS)
            {
                gpio_set_level(ATX_PS_ON_PIN, 1);
                ps_on_satus = 1;
                s_current_state = POWER_STATE_STANDBY;
            }
            break;
        }

        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ATX控制页面处理
static esp_err_t http_handler_atx_page(httpd_req_t *req)
{
    const char atx_html[] = R"rawstring(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ATX Power Control</title>
    <link rel="icon" type="image/png" href="/favicon.png">
    <style>
        * {
            margin: 0;
            padding: 0;
            box-sizing: border-box;
        }
        
        body {
            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            background: linear-gradient(135deg, #2c3e50, #4a6491, #2c3e50);
            color: #fff;
            line-height: 1.6;
            min-height: 100vh;
            padding: 20px;
        }
        
        .container {
            max-width: 800px;
            margin: 0 auto;
            padding: 20px;
        }
        
        header {
            text-align: center;
            padding: 30px 0;
            margin-bottom: 30px;
            background: rgba(0, 0, 0, 0.3);
            border-radius: 15px;
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);
            backdrop-filter: blur(10px);
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        
        h1 {
            font-size: 2.5rem;
            margin-bottom: 10px;
            text-shadow: 0 2px 4px rgba(0, 0, 0, 0.5);
        }
        
        .back-link {
            display: inline-block;
            margin-bottom: 20px;
            color: #64b5f6;
            text-decoration: none;
            font-size: 16px;
            padding: 10px 15px;
            border-radius: 5px;
            background: rgba(100, 181, 246, 0.1);
            transition: all 0.3s ease;
        }
        
        .back-link:hover {
            background: rgba(100, 181, 246, 0.2);
            text-decoration: underline;
        }
        
        .controls {
            text-align: center;
            margin: 30px 0;
        }
        
        .btn {
            font-size: 1.2rem;
            padding: 15px 30px;
            margin: 0 15px;
            border: none;
            border-radius: 8px;
            cursor: pointer;
            transition: all 0.3s ease;
            font-weight: bold;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
        }
        
        .btn:disabled {
            opacity: 0.6;
            cursor: not-allowed;
        }
        
        .btn-start {
            background: linear-gradient(to right, #4CAF50, #8BC34A);
            color: white;
        }
        
        .btn-start:hover:not(:disabled) {
            transform: translateY(-3px);
            box-shadow: 0 6px 8px rgba(0, 0, 0, 0.2);
        }
        
        .btn-shutdown {
            background: linear-gradient(to right, #f44336, #ff9800);
            color: white;
        }
        
        .btn-shutdown:hover:not(:disabled) {
            transform: translateY(-3px);
            box-shadow: 0 6px 8px rgba(0, 0, 0, 0.2);
        }
        
        .status-container {
            margin: 30px 0;
        }
        
        .status-header {
            font-size: 1.5rem;
            margin-bottom: 15px;
            color: #64b5f6;
        }
        
        .status {
            padding: 20px;
            background: rgba(0, 0, 0, 0.2);
            border-radius: 10px;
            min-height: 150px;
            max-height: 300px;
            overflow-y: auto;
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        
        .status-item {
            margin: 10px 0;
            padding: 10px;
            background: rgba(255, 255, 255, 0.05);
            border-radius: 5px;
        }
        
        .status-key {
            font-weight: bold;
            color: #64b5f6;
        }
        
        .loading {
            color: #bbbbbb;
            font-style: italic;
        }
        
        .error {
            color: #f44336;
        }
        
        .success {
            color: #4CAF50;
        }
        
        @media (max-width: 600px) {
            .btn {
                display: block;
                width: 100%;
                margin: 10px 0;
            }
            
            h1 {
                font-size: 2rem;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>ATX Power Control Panel</h1>
            <p>Remote control your computer power</p>
        </header>
        
        <a href="/" class="back-link">&larr; Back to Home</a>
        
        <div class="controls">
            <button class="btn btn-start" id="startBtn" onclick="sendCommand('start')">Power On</button>
            <button class="btn btn-shutdown" id="shutdownBtn" onclick="sendCommand('shutdown')">Power Off</button>
        </div>
        
        <div class="status-container">
            <div class="status-header">System Status</div>
            <div class="status" id="status">
                Getting status...
            </div>
        </div>
        
        <script>
            document.addEventListener('DOMContentLoaded', function() {
                updateStatus();
                setInterval(updateStatus, 5000);
            });

            function updateStatus() {
                const statusDiv = document.getElementById('status');
                statusDiv.innerHTML = '<div class="loading">Getting status...</div>';
                
                fetch('/state')
                    .then(response => {
                        if (!response.ok) {
                            throw new Error('HTTP error: ' + response.status);
                        }
                        return response.json();
                    })
                    .then(data => {
                        let statusHTML = '';
                        if (data && typeof data === 'object') {
                            for (const [key, value] of Object.entries(data)) {
                                statusHTML += '<div class="status-item"><span class="status-key">' + key + ':</span> ' + value + '</div>';
                            }
                        } else {
                            statusHTML = '<div class="error">Invalid status data</div>';
                        }
                        statusDiv.innerHTML = statusHTML;
                    })
                    .catch(error => {
                        statusDiv.innerHTML = '<div class="error">Failed to get status: ' + error.message + '</div>';
                    });
            }

            function sendCommand(action) {
                const startBtn = document.getElementById('startBtn');
                const shutdownBtn = document.getElementById('shutdownBtn');
                
                // Disable buttons to prevent multiple clicks
                startBtn.disabled = true;
                shutdownBtn.disabled = true;
                
                fetch('/control/' + action)
                    .then(response => {
                        if (!response.ok) {
                            return response.text().then(text => { throw new Error(text); });
                        }
                        return response.json();
                    })
                    .then(data => {
                        alert(data.msg || 'Command sent successfully');
                        updateStatus(); // Update status immediately
                    })
                    .catch(error => {
                        alert('Command execution failed: ' + error.message);
                    })
                    .finally(() => {
                        // Re-enable buttons
                        setTimeout(() => {
                            startBtn.disabled = false;
                            shutdownBtn.disabled = false;
                        }, 1000);
                    });
            }
        </script>
    </div>
</body>
</html>
)rawstring";

    // 设置正确的HTTP头
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Type", "text/html; charset=utf-8");

    // 发送响应
    return httpd_resp_send(req, atx_html, HTTPD_RESP_USE_STRLEN);
}

// 远程控制处理
esp_err_t http_handler_control_start(httpd_req_t *req)
{
    if (s_current_state == POWER_STATE_STANDBY)
    {
        xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_SHORT_PRESS}, 100 / portTICK_PERIOD_MS);
        httpd_resp_send(req, "{\"status\":\"success\",\"msg\":\"开机命令已发送\"}", -1);
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "当前状态不支持开机");
    }
    return ESP_OK;
}

esp_err_t http_handler_control_shutdown(httpd_req_t *req)
{
    if (s_current_state == POWER_STATE_RUNNING)
    {
        xQueueSend(s_event_queue, &(AppEvent_t){APP_EVENT_PWR_SW_LONG_HOLD}, 100 / portTICK_PERIOD_MS);
        httpd_resp_send(req, "{\"status\":\"success\",\"msg\":\"关机命令已发送\"}", -1);
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "当前状态不支持关机");
    }
    return ESP_OK;
}

// ATX模块路由
static const HttpRoute_t s_atx_routes[] = {
    {.uri = "/atx", .method = HTTP_GET, .handler = http_handler_atx_page, .user_ctx = NULL},
    {.uri = "/control/start", .method = HTTP_GET, .handler = http_handler_control_start, .user_ctx = NULL},
    {.uri = "/control/shutdown", .method = HTTP_GET, .handler = http_handler_control_shutdown, .user_ctx = NULL},
};

// ATX状态回调（给Web模块）
static char *atx_state_callback(void)
{
    char *state = (char *)malloc(256);
    const char *state_str[] = {"待机", "启动中", "运行中", "主板关机中", "电源关机中"};
    portENTER_CRITICAL(&s_state_mutex);
    snprintf(state, 256, "\"系统状态\": \"%s\",\n  \"主板电源\": \"%s\",\n  \"ATX电源\": \"%s\"",
             state_str[s_current_state],
             s_mainboard_power ? "开机" : "关机",
             ps_on_satus ? "待机(拉高)" : "供电(拉低)");
    portEXIT_CRITICAL(&s_state_mutex);
    return state;
}

esp_err_t atx_controller_init(void)
{
    gpio_init_config();
    s_event_queue = xQueueCreate(10, sizeof(AppEvent_t)); // 增加队列长度
    if (!s_event_queue)
        return ESP_ERR_NO_MEM;

    // 注册状态回调
    esp_err_t ret = common_register_state_callback(atx_state_callback);
    if (ret != ESP_OK)
        return ret;

    ret = http_server_register_routes(s_atx_routes, sizeof(s_atx_routes) / sizeof(HttpRoute_t));
    if (ret != ESP_OK)
        return ret;

    // 注册模块链接
    ret = http_server_register_module_link("ATX电源控制", "/atx");
    if (ret != ESP_OK)
        return ret;

    // 创建任务 - 取消注释并调整栈大小
    xTaskCreate(monitor_task, "atx_monitor", 3072, NULL, 5, NULL);
    xTaskCreate(control_task, "atx_control", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "ATX控制器初始化完成");
    return ESP_OK;
}