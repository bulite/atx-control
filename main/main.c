#include "common.h"
#include "wifi_manager.h"
#include "http_server.h"
#include "ota_manager.h"
#include "atx_controller.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "main";

void app_main(void) {

    esp_err_t ret = common_log_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "日志初始化失败：%s", esp_err_to_name(ret));

    ESP_LOGI(TAG, "=====================================");
    ESP_LOGI(TAG, "ESP32 ATX电源控制器（模块化版）");
    ESP_LOGI(TAG, "Web登录：%s/%s", AUTH_USERNAME, AUTH_PASSWORD);
    ESP_LOGI(TAG, "远程控制密钥：%s", CONTROL_SECRET_KEY);
    ESP_LOGI(TAG, "=====================================");

    // 模块初始化（顺序：公共工具 → WiFi → HTTP → OTA → ATX）
    ret = wifi_manager_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "WiFi初始化失败：%s", esp_err_to_name(ret));

    ret = http_server_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "HTTP服务器初始化失败：%s", esp_err_to_name(ret));

    ret = ota_manager_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "OTA初始化失败：%s", esp_err_to_name(ret));
    ret = ota_manager_register_web_routes();
    if (ret != ESP_OK) ESP_LOGE(TAG, "OTA路由注册失败：%s", esp_err_to_name(ret));

    ret = atx_controller_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "ATX控制器初始化失败：%s", esp_err_to_name(ret));
    ret = atx_controller_register_web_routes();
    if (ret != ESP_OK) ESP_LOGE(TAG, "ATX路由注册失败：%s", esp_err_to_name(ret));

    ESP_LOGI(TAG, "所有模块初始化完成，系统启动成功");
}