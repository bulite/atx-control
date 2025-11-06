#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <esp_err.h>
#include <stdbool.h>

// WiFi配置（用户需修改）
#define WIFI_SSID           "Xiaomi_90D3"
#define WIFI_PASSWORD       "108117wcj"
#define WIFI_RETRY_COUNT    5
#define WIFI_CONNECT_TIMEOUT 10000

esp_err_t wifi_manager_init(void);
bool wifi_manager_is_connected(void);
char* wifi_manager_get_ip_addr(void);

#endif // WIFI_MANAGER_H