#include "ota_manager.h"
#include "common.h"
#include "http_server.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "string.h"

typedef enum
{
    OTA_STATUS_IDLE,
    OTA_STATUS_UPDATING,
    OTA_STATUS_SUCCESS,
    OTA_STATUS_FAILED
} OtaStatus_t;

static const char *TAG = "ota_manager";
static const esp_partition_t *s_boot_part = NULL;
static const esp_partition_t *s_update_part = NULL;
static OtaStatus_t s_ota_status = OTA_STATUS_IDLE;
static int s_ota_progress = 0;
static bool s_ota_upgrading = false;
static portMUX_TYPE s_ota_mutex = portMUX_INITIALIZER_UNLOCKED;

// NVS操作
static esp_err_t nvs_save_ota_state(OtaStatus_t status, const char *old_part)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_OTA, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
        return ret;

    ret = nvs_set_u8(handle, "status", status);
    if (ret != ESP_OK)
        goto exit;
    if (old_part)
        ret = nvs_set_str(handle, "old_part", old_part);
    if (ret != ESP_OK)
        goto exit;
    ret = nvs_set_u64(handle, "ts", common_get_time_ms());

exit:
    nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

static esp_err_t nvs_load_ota_state(OtaStatus_t *status, char *old_part, size_t part_len)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE_OTA, NVS_READONLY, &handle);
    if (ret != ESP_OK)
        return ret;

    uint8_t status_u8 = OTA_STATUS_IDLE;
    ret = nvs_get_u8(handle, "status", &status_u8);
    if (ret == ESP_ERR_NVS_NOT_FOUND)
        ret = ESP_OK;
    *status = (OtaStatus_t)status_u8;

    if (old_part && part_len > 0)
    {
        ret = nvs_get_str(handle, "old_part", old_part, &part_len);
        if (ret == ESP_ERR_NVS_NOT_FOUND)
        {
            old_part[0] = '\0';
            ret = ESP_OK;
        }
    }

    nvs_close(handle);
    return ret;
}

static esp_err_t nvs_erase_namespace(const char *namespace)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(namespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK)
        return ret;
    ret = nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

// 首次启动校验
static void ota_check_first_boot(void)
{
    s_boot_part = esp_ota_get_boot_partition();
    s_update_part = esp_ota_get_next_update_partition(NULL);

    OtaStatus_t status;
    char old_part[32] = {0};
    esp_err_t ret = nvs_load_ota_state(&status, old_part, sizeof(old_part));
    if (ret != ESP_OK)
        return;

    switch (status)
    {
    case OTA_STATUS_SUCCESS:
        ESP_LOGI(TAG, "新固件启动成功，清除OTA状态");
        nvs_erase_namespace(NVS_NAMESPACE_OTA);
        break;
    case OTA_STATUS_FAILED:
    case OTA_STATUS_UPDATING:
        ESP_LOGW(TAG, "OTA中断，回滚到原分区：%s", old_part);
        if (strlen(old_part) > 0)
        {
            const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, old_part);
            if (part)
                esp_ota_set_boot_partition(part);
        }
        nvs_erase_namespace(NVS_NAMESPACE_OTA);
        esp_restart();
        break;
    default:
        break;
    }
}

// OTA进度查询处理
static esp_err_t http_handler_ota_progress(httpd_req_t *req)
{
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
static esp_err_t http_handler_ota_page(httpd_req_t *req)
{
    const char ota_html[] = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>OTA固件升级</title>
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
        
        .upload-section {
            background: rgba(0, 0, 0, 0.2);
            padding: 30px;
            border-radius: 15px;
            margin: 20px 0;
            text-align: center;
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        
        .file-input {
            margin: 20px 0;
            padding: 15px;
            background: rgba(255, 255, 255, 0.1);
            border-radius: 8px;
            border: 1px dashed rgba(255, 255, 255, 0.3);
            width: 100%;
            color: #fff;
        }
        
        .upload-btn {
            background: linear-gradient(to right, #2196F3, #21CBF3);
            color: white;
            border: none;
            padding: 15px 30px;
            font-size: 1.1rem;
            border-radius: 8px;
            cursor: pointer;
            transition: all 0.3s ease;
            font-weight: bold;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.1);
        }
        
        .upload-btn:hover:not(:disabled) {
            transform: translateY(-3px);
            box-shadow: 0 6px 8px rgba(0, 0, 0, 0.2);
        }
        
        .upload-btn:disabled {
            opacity: 0.6;
            cursor: not-allowed;
        }
        
        .progress-container {
            margin: 30px 0;
        }
        
        .progress-label {
            font-size: 1.2rem;
            margin-bottom: 10px;
            color: #64b5f6;
        }
        
        .progress {
            width: 100%;
            height: 25px;
            background: rgba(0, 0, 0, 0.3);
            border-radius: 12px;
            overflow: hidden;
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        
        .progress-bar {
            height: 100%;
            background: linear-gradient(to right, #4CAF50, #8BC34A);
            width: 0%;
            transition: width 0.3s ease;
        }
        
        .status {
            margin: 20px 0;
            padding: 15px;
            background: rgba(0, 0, 0, 0.2);
            border-radius: 8px;
            text-align: center;
            min-height: 20px;
            border: 1px solid rgba(255, 255, 255, 0.1);
        }
        
        .warning {
            background: rgba(255, 152, 0, 0.2);
            border-left: 4px solid #ff9800;
            padding: 15px;
            margin: 20px 0;
            border-radius: 0 8px 8px 0;
        }
        
        @media (max-width: 600px) {
            .container {
                padding: 10px;
            }
            
            h1 {
                font-size: 2rem;
            }
            
            .upload-section {
                padding: 20px 15px;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>OTA固件升级</h1>
            <p>安全可靠地更新设备固件</p>
        </header>
        
        <a href="/" class="back-link">← 返回主页</a>
        
        <div class="warning">
            <strong>警告：</strong>固件升级过程中请勿断电或中断连接，否则可能导致设备无法正常工作。
        </div>
        
        <div class="upload-section">
            <h2>选择固件文件</h2>
            <input type="file" id="firmwareFile" class="file-input" accept=".bin" required><br>
            <button type="button" id="uploadBtn" class="upload-btn">开始升级</button>
        </div>
        
        <div class="progress-container">
            <div class="progress-label">升级进度</div>
            <div class="progress">
                <div class="progress-bar" id="progressBar"></div>
            </div>
        </div>
        
        <div class="status" id="status">准备就绪</div>
        
        <script>
            const uploadBtn = document.getElementById('uploadBtn');
            const fileInput = document.getElementById('firmwareFile');
            const progressBar = document.getElementById('progressBar');
            const statusText = document.getElementById('status');
            
            uploadBtn.addEventListener('click', () => {
                const file = fileInput.files[0];
                if (!file) {
                    statusText.textContent = '请选择一个固件文件';
                    return;
                }

                const xhr = new XMLHttpRequest();
                xhr.open('POST', '/update', true);
                xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                
                // 更新进度显示
                let interval = setInterval(() => {
                    fetch('/ota/progress').then(res => res.json()).then(data => {
                        progressBar.style.width = data.progress + '%';
                        statusText.textContent = '升级中：' + data.progress + '%';
                    });
                }, 500);

                xhr.upload.addEventListener('progress', (e) => {
                    if (e.lengthComputable) {
                        const percentComplete = (e.loaded / e.total) * 100;
                        progressBar.style.width = percentComplete + '%';
                        statusText.textContent = '上传中：' + Math.round(percentComplete) + '%';
                    }
                });

                xhr.onload = () => {
                    clearInterval(interval);
                    if (xhr.status === 200) {
                        statusText.textContent = '升级成功，重启中...';
                        setTimeout(() => {
                            location.reload();
                        }, 3000);
                    } else {
                        statusText.textContent = '升级失败: ' + xhr.statusText;
                    }
                };

                xhr.onerror = () => {
                    clearInterval(interval);
                    statusText.textContent = '上传失败';
                };

                xhr.send(file);
                statusText.textContent = '开始上传...';
                uploadBtn.disabled = true;
            });
        </script>
    </div>
</body>
</html>
)";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, ota_html, strlen(ota_html));
    return ESP_OK;
}

// OTA升级处理
static esp_err_t http_handler_ota_update(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_ota_mutex);
    if (s_ota_upgrading)
    {
        portEXIT_CRITICAL(&s_ota_mutex);
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "升级中，请勿重复请求");
        return ESP_OK;
    }
    s_ota_status = OTA_STATUS_UPDATING;
    s_ota_upgrading = true;
    s_ota_progress = 0;
    portEXIT_CRITICAL(&s_ota_mutex);

    esp_err_t ret = ESP_OK;

    if (!s_update_part)
        s_update_part = esp_ota_get_next_update_partition(NULL);
    if (!s_update_part || !s_boot_part)
    {
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ret = nvs_save_ota_state(OTA_STATUS_UPDATING, s_boot_part->label);
    if (ret != ESP_OK)
        goto fail;

    int content_len = req->content_len;
    esp_ota_handle_t ota_handle = 0;
    char ota_buf[OTA_BUFFER_SIZE];
    ret = esp_ota_begin(s_update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (ret != ESP_OK)
        goto fail;

    ESP_LOGI(TAG, "OTA升级开始，固件大小：%d字节", content_len);

    // 添加计数器来跟踪已接收的字节数
    int total_received = 0;

    while (content_len > 0 && ret == ESP_OK)
    {
        int recv_len = MIN(content_len, OTA_BUFFER_SIZE);
        int remaining = recv_len;
        int received = 0;

        // 循环接收直到获得完整的数据块或出错
        while (remaining > 0)
        {
            ret = httpd_req_recv(req, ota_buf + received, remaining);
            if (ret <= 0)
            {
                if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                {
                    // 超时重试
                    continue;
                }
                ESP_LOGE(TAG, "接收数据失败: %d", ret);
                break;
            }
            received += ret;
            remaining -= ret;
        }

        if (ret <= 0)
            break;

        // 添加调试日志，查看前几个字节
        if (total_received == 0)
        {
            ESP_LOGI(TAG, "固件开头数据 (16字节): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                     ota_buf[0], ota_buf[1], ota_buf[2], ota_buf[3],
                     ota_buf[4], ota_buf[5], ota_buf[6], ota_buf[7],
                     ota_buf[8], ota_buf[9], ota_buf[10], ota_buf[11],
                     ota_buf[12], ota_buf[13], ota_buf[14], ota_buf[15]);
        }

        ret = esp_ota_write(ota_handle, ota_buf, received);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "写入OTA数据失败: %s", esp_err_to_name(ret));
            break;
        }

        content_len -= received;
        total_received += received;

        portENTER_CRITICAL(&s_ota_mutex);
        s_ota_progress = (int)(((float)(req->content_len - content_len) / req->content_len) * 100);
        portEXIT_CRITICAL(&s_ota_mutex);
    }

    ESP_LOGI(TAG, "总共接收数据: %d 字节", total_received);

    if (ret == ESP_OK)
    {
        ret = esp_ota_end(ota_handle);
        if (ret == ESP_OK)
        {
            ret = esp_ota_set_boot_partition(s_update_part);
            if (ret == ESP_OK)
            {
                nvs_save_ota_state(OTA_STATUS_SUCCESS, s_boot_part->label);
                portENTER_CRITICAL(&s_ota_mutex);
                s_ota_status = OTA_STATUS_SUCCESS;
                s_ota_progress = 100;
                portEXIT_CRITICAL(&s_ota_mutex);
                httpd_resp_send(req, "升级成功，重启中...", -1);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                esp_restart();
            }
            else
            {
                ESP_LOGE(TAG, "设置启动分区失败: %s", esp_err_to_name(ret));
            }
        }
        else
        {
            ESP_LOGE(TAG, "OTA结束验证失败: %s", esp_err_to_name(ret));
        }
    }

fail:
    if (ota_handle)
        esp_ota_abort(ota_handle);
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
static char *ota_state_callback(void)
{
    char *state = (char *)malloc(256);
    portENTER_CRITICAL(&s_ota_mutex);
    const char *status_str[] = {"未升级", "升级中", "升级成功", "升级失败"};
    snprintf(state, 256, "  \"OTA状态\": \"%s\",\n  \"OTA进度\": %d,\n  \"OTA升级中\": %s",
             status_str[s_ota_status], s_ota_progress, s_ota_upgrading ? "true" : "false");
    portEXIT_CRITICAL(&s_ota_mutex);
    return state;
}

esp_err_t ota_manager_init(void)
{
    s_boot_part = esp_ota_get_boot_partition();
    s_update_part = esp_ota_get_next_update_partition(NULL);
    ota_check_first_boot();

    // 注册路由
    esp_err_t ret = http_server_register_routes(s_ota_routes, sizeof(s_ota_routes) / sizeof(HttpRoute_t));
    if (ret != ESP_OK)
        return ret;

    ret = http_server_register_module_link("OTA固件升级", "/update");
    if (ret != ESP_OK)
        return ret;

    // 注册状态回调
    ret = common_register_state_callback(ota_state_callback);
    if (ret != ESP_OK)
        return ret;

    vTaskDelay(5000 / portTICK_PERIOD_MS);

    // 新固件启动后确认成功（避免回滚，需配合menuconfig的回滚配置）
    ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "新固件确认成功，取消回滚");
    }
    else
    {
        ESP_LOGE(TAG, "确认失败，将触发回滚");
    }

    return ESP_OK;
}
