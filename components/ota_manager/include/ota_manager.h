#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <esp_err.h>
#include "http_server.h"

// OTA配置
#define OTA_BUFFER_SIZE     4096
#define NVS_NAMESPACE_OTA   "ota_state"

esp_err_t ota_manager_init(void);

#endif // OTA_MANAGER_H