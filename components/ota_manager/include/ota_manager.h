#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <esp_err.h>
#include "http_server.h"

// OTA配置
#define OTA_BUFFER_SIZE     4096
#define OTA_MD5_ENABLE      1
#define NVS_NAMESPACE_OTA   "ota_state"

typedef enum {
    OTA_STATUS_IDLE,
    OTA_STATUS_UPDATING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED
} OtaStatus_t;

esp_err_t ota_manager_init(void);
OtaStatus_t ota_manager_get_status(void);
int ota_manager_get_progress(void);
bool ota_manager_is_upgrading(void);
esp_err_t ota_manager_register_web_routes(void);

#endif // OTA_MANAGER_H