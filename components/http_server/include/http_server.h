#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <esp_err.h>
#include <esp_http_server.h>

// Web配置
#define HTTP_SERVER_PORT    80

// 模块链接结构体
typedef struct {
    const char *name;
    const char *url;
} ModuleLink_t;

// 路由注册结构体
typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_handle_t handler;
    void *user_ctx;
} HttpRoute_t;

esp_err_t http_server_init(void);
esp_err_t http_server_register_routes(const HttpRoute_t *routes, size_t route_count);
esp_err_t http_server_register_module_link(const char *name, const char *url);

#endif // HTTP_SERVER_H