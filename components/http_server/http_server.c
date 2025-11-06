#include "http_server.h"
#include "common.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "http_server";
static httpd_handle_t s_httpd_handle = NULL;
static HttpStateCallback_t s_state_callbacks[5] = {NULL}; // 支持最多5个模块状态回调
static size_t s_callback_count = 0;

// 基础页面模板
static const char s_root_html[] = R"(
<!DOCTYPE html>
<html>
<head><title>ATX电源控制器</title></head>
<body>
    <h1>ESP32 ATX电源控制器</h1>
    <h3>功能导航（登录：admin/admin123）</h3>
    <ul>
        <li><a href="/log" target="_blank">实时日志监控</a></li>
        <li><a href="/state">系统状态查询</a></li>
        <li><a href="/update">OTA固件升级</a></li>
    </ul>
    <h3>远程控制</h3>
    <p>开机：/control/start?key=你的密钥</p>
    <p>关机：/control/shutdown?key=你的密钥</p>
</body>
</html>
)";

// 根路径处理
static esp_err_t http_handler_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_root_html, strlen(s_root_html));
    return ESP_OK;
}

// 日志监控处理
static esp_err_t http_handler_log(httpd_req_t *req) {
    if (!http_server_auth_verify(req)) return ESP_OK;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    ESP_LOGI(TAG, "日志连接建立：%s", req->client_ip);
    while (1) {
        size_t read_len = 0;
        char *log_data = common_log_buffer_read(&read_len);
        if (read_len > 0) {
            if (httpd_resp_send_chunk(req, log_data, read_len) != ESP_OK) break;
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
        if (httpd_req_check_stop(req) == ESP_ERR_HTTPD_REQUEST_CLOSED) break;
    }
    ESP_LOGI(TAG, "日志连接断开：%s", req->client_ip);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// 系统状态聚合处理
static esp_err_t http_handler_state(httpd_req_t *req) {
    if (!http_server_auth_verify(req)) return ESP_OK;

    char state_buf[2048] = "{\n";
    strcat(state_buf, "  \"WiFi状态\": \"");
    strcat(state_buf, wifi_manager_is_connected() ? "已连接" : "未连接");
    strcat(state_buf, "\",\n");
    strcat(state_buf, "  \"IP地址\": \"");
    strcat(state_buf, wifi_manager_get_ip_addr());
    strcat(state_buf, "\",\n");

    // 调用各模块状态回调
    for (size_t i = 0; i < s_callback_count; i++) {
        if (s_state_callbacks[i]) {
            char *module_state = s_state_callbacks[i]();
            if (module_state) {
                strcat(state_buf, module_state);
                free(module_state);
                strcat(state_buf, ",\n");
            }
        }
    }

    // 移除最后一个逗号
    if (state_buf[strlen(state_buf)-2] == ',') {
        state_buf[strlen(state_buf)-2] = '\n';
    }
    strcat(state_buf, "}");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, state_buf, strlen(state_buf));
    return ESP_OK;
}

// 基础路由（根路径、日志、状态）
static const HttpRoute_t s_base_routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = http_handler_root, .user_ctx = NULL},
    {.uri = "/log", .method = HTTP_GET, .handler = http_handler_log, .user_ctx = NULL},
    {.uri = "/state", .method = HTTP_GET, .handler = http_handler_state, .user_ctx = NULL},
};

bool http_server_auth_verify(httpd_req_t *req) {
    const char *auth_header = "Authorization";
    char auth_buf[128] = {0};
    char decoded_buf[64] = {0};
    char expected[64] = {0};
    snprintf(expected, sizeof(expected), "%s:%s", AUTH_USERNAME, AUTH_PASSWORD);

    if (httpd_req_get_hdr_value_str(req, auth_header, auth_buf, sizeof(auth_buf)) != ESP_OK) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ATX电源控制器\"");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "需要身份验证");
        return false;
    }

    if (strncmp(auth_buf, "Basic ", 6) != 0) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "认证格式错误");
        return false;
    }

    if (!common_base64_decode(auth_buf + 6, strlen(auth_buf)-6, decoded_buf, sizeof(decoded_buf)) ||
        strcmp(decoded_buf, expected) != 0) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "用户名或密码错误");
        return false;
    }

    return true;
}

esp_err_t http_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.task_stack_size = 4096;
    config.max_open_sockets = 5;

    if (httpd_start(&s_httpd_handle, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP服务器启动失败");
        return ESP_FAIL;
    }

    // 注册基础路由
    http_server_register_routes(s_base_routes, sizeof(s_base_routes)/sizeof(HttpRoute_t));
    ESP_LOGI(TAG, "HTTP服务器启动成功，端口：%d", HTTP_SERVER_PORT);
    return ESP_OK;
}

esp_err_t http_server_register_routes(const HttpRoute_t *routes, size_t route_count) {
    if (!s_httpd_handle || !routes || route_count == 0) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < route_count; i++) {
        httpd_register_uri_handler(s_httpd_handle, &routes[i]);
        ESP_LOGD(TAG, "注册路由：%s %s",
                 routes[i].method == HTTP_GET ? "GET" : "POST", routes[i].uri);
    }
    return ESP_OK;
}

esp_err_t http_server_register_state_callback(HttpStateCallback_t callback) {
    if (!callback || s_callback_count >= sizeof(s_state_callbacks)/sizeof(s_state_callbacks[0])) {
        return ESP_ERR_NO_MEM;
    }
    s_state_callbacks[s_callback_count++] = callback;
    return ESP_OK;
}