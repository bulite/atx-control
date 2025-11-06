#include "ota_manager.h"
#include "common.h"
#include "http_server.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "ota_manager";
static const esp_partition_t *s_boot_part = NULL;
static const esp_partition_t *s_update_part = NULL;
static OtaStatus_t s_ota_status = OTA_STATUS_IDLE;
static int s_ota_progress = 0;
static bool s_ota_upgrading = false;
static portMUX_TYPE s_ota_mutex = portMUX_INITIALIZER_UNLOCKED;

// NVS操作
static esp_err_t nvs_save_ota_state(OtaStatus_t status, const char *old_part) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_OTA, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_u8(handle, "status", status);
    if (ret != ESP_OK) goto exit;
    if (old_part) ret = nvs_set_str(handle, "old_part", old_part);
    if (ret != ESP_OK) goto exit;
    ret = nvs_set_u64(handle, "ts", common_get_time_ms());

exit:
    nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

static esp_err_t nvs_load_ota_state(OtaStatus_t *status, char *old_part, size_t part_len) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_OTA, NVS_READONLY, &handle);
    if (ret != ESP_OK) return ret;

    uint8_t status_u8 = OTA_STATUS_IDLE;
    ret = nvs_get_u8(handle, "status", &status_u8);
    if (ret == ESP_ERR_NVS_NOT_FOUND) ret = ESP_OK;
    *status = (OtaStatus_t)status_u8;

    if (old_part && part_len > 0) {
        ret = nvs_get_str(handle, "old_part", old_part, &part_len);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            old_part[0] = '\0';
            ret = ESP_OK;
        }
    }

    nvs_close(handle);
    return ret;
}

// 首次启动校验
static void ota_check_first_boot(void) {
    s_boot_part = esp_ota_get_boot_partition();
    s_update_part = esp_ota_get_next_update_partition(NULL);

    OtaStatus_t status;
    char old_part[32] = {0};
    esp_err_t ret = nvs_load_ota_state(&status, old_part, sizeof(old_part));
    if (ret != ESP_OK) return;

    switch (status) {
        case OTA_STATUS_SUCCESS:
            ESP_LOGI(TAG, "新固件启动成功，清除OTA状态");
            nvs_erase_all_from_namespace(NVS_NAMESPACE_OTA);
            break;
        case OTA_STATUS_FAILED:
        case OTA_STATUS_UPDATING:
            ESP_LOGW(TAG, "OTA中断，回滚到原分区：%s", old_part);
            if (strlen(old_part) > 0) {
                const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, old_part);
                if (part) esp_ota_set_boot_partition(part);
            }
            nvs_erase_all_from_namespace(NVS_NAMESPACE_OTA);
            esp_restart();
            break;
        default:
            break;
    }
}

// OTA进度查询处理
static esp_err_t http_handler_ota_progress(httpd_req_t *req) {
    if (!http_server_auth_verify(req)) return ESP_OK;

    char resp[64];
    portENTER_CRITICAL(&s_ota_mutex);
    snprintf(resp, sizeof(resp), "{\"progress\":%d,\"status\":\"%s\"}",
             s_ota_progress, s_ota_upgrading ? "updating" : "idle");
    portEXIT_CRITICAL(&s_ota_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

// OTA升级页面处理
static esp_err_t http_handler_ota_page(httpd_req_t *req) {
    if (!http_server_auth_verify(req)) return ESP_OK;

    const char ota_html[] = R"(
<!DOCTYPE html>
<html>
<head>
    <title>OTA升级</title>
    <style>
        .progress {width:500px;height:20px;border:1px solid #ccc;margin:20px 0;}
        .progress-bar {height:100%;background:#4CAF50;width:0%;transition:width 0.3s;}
    </style>
</head>
<body>
    <h1>OTA固件升级</h1>
    <form id="uploadForm" method="post" action="/update" enctype="multipart/form-data">
        <input type="file" name="firmware" accept=".bin" required><br><br>
        <button type="submit">开始升级</button>
    </form>
    <div id="status">未开始</div>
    <div class="progress"><div class="progress-bar" id="progressBar"></div></div>
    <script>
        const form = document.getElementById('uploadForm');
        const progressBar = document.getElementById('progressBar');
        const statusText = document.getElementById('status');
        let interval;
        form.addEventListener('submit', e => {
            e.preventDefault();
            const formData = new FormData(form);
            const xhr = new XMLHttpRequest();
            xhr.open('POST', '/update', true);
            statusText.textContent = '上传中...';
            interval = setInterval(() => {
                fetch('/ota/progress').then(res => res.json()).then(data => {
                    progressBar.style.width = data.progress + '%';
                    statusText.textContent = '升级中：' + data.progress + '%';
                });
            }, 500);
            xhr.onload = () => {
                clearInterval(interval);
                statusText.textContent = xhr.status == 200 ? '升级成功，重启中...' : '升级失败';
            };
            xhr.onerror = () => {
                clearInterval(interval);
                statusText.textContent = '上传失败';
            };
            xhr.send(formData);
        });
    </script>
</body>
</html>
)";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ota_html, strlen(ota_html));
    return ESP_OK;
}

// OTA升级处理
static esp_err_t http_handler_ota_update(httpd_req_t *req) {
    if (!http_server_auth_verify(req)) return ESP_OK;

    portENTER_CRITICAL(&s_ota_mutex);
    if (s_ota_upgrading) {
        portEXIT_CRITICAL(&s_ota_mutex);
        httpd_resp_send_err(req, HTTPD_409_CONFLICT, "升级中，请勿重复请求");
        return ESP_OK;
    }
    s_ota_status = OTA_STATUS_UPDATING;
    s_ota_upgrading = true;
    s_ota_progress = 0;
    portEXIT_CRITICAL(&s_ota_mutex);

    esp_err_t ret = ESP_OK;
    char firmware_md5[33] = {0};
#if OTA_MD5_ENABLE
    if (httpd_req_get_hdr_value_str(req, "X-Firmware-MD5", firmware_md5, sizeof(firmware_md5)) != ESP_OK) {
        ret = ESP_ERR_INVALID_ARG;
        goto fail;
    }
    if (strlen(firmware_md5) != 32) {
        ret = ESP_ERR_INVALID_ARG;
        goto fail;
    }
#endif

    if (!s_update_part) s_update_part = esp_ota_get_next_update_partition(NULL);
    if (!s_update_part || !s_boot_part) {
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ret = nvs_save_ota_state(OTA_STATUS_UPDATING, s_boot_part->label);
    if (ret != ESP_OK) goto fail;

    int content_len = req->content_len;
    esp_ota_handle_t ota_handle = 0;
    char ota_buf[OTA_BUFFER_SIZE];
    ret = esp_ota_begin(s_update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (ret != ESP_OK) goto fail;

#if OTA_MD5_ENABLE
    md5_context_t md5_ctx;
    md5_init(&md5_ctx);
#endif

    ESP_LOGI(TAG, "OTA升级开始，固件大小：%d字节", content_len);
    while (content_len > 0 && ret == ESP_OK) {
        int recv_len = MIN(content_len, OTA_BUFFER_SIZE);
        ret = httpd_req_recv(req, ota_buf, recv_len);
        if (ret <= 0) break;

#if OTA_MD5_ENABLE
        md5_update(&md5_ctx, (uint8_t *)ota_buf, recv_len);
#endif
        ret = esp_ota_write(ota_handle, ota_buf, recv_len);
        if (ret != ESP_OK) break;

        content_len -= recv_len;
        portENTER_CRITICAL(&s_ota_mutex);
        s_ota_progress = (int)(((float)(req->content_len - content_len) / req->content_len) * 100);
        portEXIT_CRITICAL(&s_ota_mutex);
    }

    if (ret == ESP_OK) {
#if OTA_MD5_ENABLE
        uint8_t md5_result[16];
        md5_final(&md5_ctx, md5_result);
        char md5_str[33] = {0};
        for (int i = 0; i < 16; i++) snprintf(&md5_str[i*2], 3, "%02x", md5_result[i]);
        if (strcasecmp(md5_str, firmware_md5) != 0) {
            ESP_LOGE(TAG, "MD5校验失败");
            ret = ESP_ERR_INVALID_CRC;
            goto fail;
        }
#endif

        ret = esp_ota_end(ota_handle);
        if (ret == ESP_OK) {
            ret = esp_ota_set_boot_partition(s_update_part);
            if (ret == ESP_OK) {
                nvs_save_ota_state(OTA_STATUS_SUCCESS, s_boot_part->label);
                portENTER_CRITICAL(&s_ota_mutex);
                s_ota_status = OTA_STATUS_SUCCESS;
                s_ota_progress = 100;
                portEXIT_CRITICAL(&s_ota_mutex);
                httpd_resp_send(req, "升级成功，重启中...", -1);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_restart();
            }
        }
    }

fail:
    if (ota_handle) esp_ota_abort(ota_handle);
    nvs_save_ota_state(OTA_STATUS_FAILED, s_boot_part->label);
    portENTER_CRITICAL(&s_ota_mutex);
    s_ota_status = OTA_STATUS_FAILED;
    s_ota_upgrading = false;
    s_ota_progress = 0;
    portEXIT_CRITICAL(&s_ota_mutex);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "升级失败");
    return ESP_OK;
}

// OTA模块路由
static const HttpRoute_t s_ota_routes[] = {
    {.uri = "/update", .method = HTTP_GET, .handler = http_handler_ota_page, .user_ctx = NULL},
    {.uri = "/update", .method = HTTP_POST, .handler = http_handler_ota_update, .user_ctx = NULL},
    {.uri = "/ota/progress", .method = HTTP_GET, .handler = http_handler_ota_progress, .user_ctx = NULL},
};

// OTA状态回调（给Web模块）
static char* ota_state_callback(void) {
    char *state = (char *)malloc(256);
    portENTER_CRITICAL(&s_ota_mutex);
    const char *status_str[] = {"未升级", "升级中", "升级成功", "升级失败"};
    snprintf(state, 256, "  \"OTA状态\": \"%s\",\n  \"OTA进度\": %d,\n  \"OTA升级中\": %s",
             status_str[s_ota_status], s_ota_progress, s_ota_upgrading ? "true" : "false");
    portEXIT_CRITICAL(&s_ota_mutex);
    return state;
}

esp_err_t ota_manager_init(void) {
    s_boot_part = esp_ota_get_boot_partition();
    s_update_part = esp_ota_get_next_update_partition(NULL);
    ota_check_first_boot();
    return http_server_register_state_callback(ota_state_callback);
}

OtaStatus_t ota_manager_get_status(void) {
    portENTER_CRITICAL(&s_ota_mutex);
    OtaStatus_t status = s_ota_status;
    portEXIT_CRITICAL(&s_ota_mutex);
    return status;
}

int ota_manager_get_progress(void) {
    portENTER_CRITICAL(&s_ota_mutex);
    int progress = s_ota_progress;
    portEXIT_CRITICAL(&s_ota_mutex);
    return progress;
}

bool ota_manager_is_upgrading(void) {
    portENTER_CRITICAL(&s_ota_mutex);
    bool upgrading = s_ota_upgrading;
    portEXIT_CRITICAL(&s_ota_mutex);
    return upgrading;
}

esp_err_t ota_manager_register_web_routes(void) {
    return http_server_register_routes(s_ota_routes, sizeof(s_ota_routes)/sizeof(HttpRoute_t));
}