#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <esp_err.h>
#include <esp_http_server.h>

// Web配置
#define HTTP_SERVER_PORT    80
#define AUTH_USERNAME       "admin"
#define AUTH_PASSWORD       "admin123"

// 路由注册结构体
typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_handle_t handler;
    void *user_ctx;
} HttpRoute_t;

// 状态回调函数类型（用于Web页面获取各模块状态）
typedef char* (*HttpStateCallback_t)(void);

esp_err_t http_server_init(void);
esp_err_t http_server_register_routes(const HttpRoute_t *routes, size_t route_count);
esp_err_t http_server_register_state_callback(HttpStateCallback_t callback);
bool http_server_auth_verify(httpd_req_t *req);

#endif // HTTP_SERVER_H