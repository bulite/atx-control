#include <sys/socket.h>
#include <errno.h>
#include "common.h"
#include "http_server.h"


static const char *TAG = "http_server";
static httpd_handle_t s_httpd_handle = NULL;
static ModuleLink_t s_module_links[10] = {0}; // 支持最多10个模块链接
static size_t s_link_count = 0;

// 基础页面模板
static const char s_root_html_head[] = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ATX电源控制器</title>
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
        
        h3 {
            font-size: 1.5rem;
            margin-bottom: 20px;
            color: #64b5f6;
        }
        
        .modules-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 20px;
            margin-top: 20px;
        }
        
        .module-card {
            background: rgba(255, 255, 255, 0.1);
            border-radius: 15px;
            padding: 25px;
            transition: all 0.3s ease;
            border: 1px solid rgba(255, 255, 255, 0.15);
            backdrop-filter: blur(10px);
        }
        
        .module-card:hover {
            transform: translateY(-5px);
            box-shadow: 0 12px 20px rgba(0, 0, 0, 0.4);
            background: rgba(255, 255, 255, 0.15);
            border-color: rgba(100, 181, 246, 0.5);
        }
        
        .module-card a {
            text-decoration: none;
            color: inherit;
            display: block;
        }
        
        .module-title {
            font-size: 1.3rem;
            font-weight: 600;
            margin-bottom: 10px;
            color: #64b5f6;
            display: flex;
            align-items: center;
        }
        
        .module-title::before {
            content: "▶";
            margin-right: 10px;
            font-size: 1.2rem;
        }
        
        .module-description {
            color: #e0e0e0;
            font-size: 1rem;
        }
        
        footer {
            text-align: center;
            margin-top: 40px;
            padding: 20px;
            color: #bbbbbb;
            font-size: 0.9rem;
        }
        
        @media (max-width: 600px) {
            .container {
                padding: 10px;
            }
            
            h1 {
                font-size: 2rem;
            }
            
            h3 {
                font-size: 1.2rem;
            }
            
            .modules-grid {
                grid-template-columns: 1fr;
            }
        }
    </style>
</head>
<body>
    <div class="container">
        <header>
            <h1>ESP32 ATX电源控制器</h1>
            <p>专业级电源控制系统</p>
        </header>
        
        <main>
            <h3>功能导航</h3>
            <div class="modules-grid">
)";

static const char s_root_html_tail[] = R"(
            </div>
        </main>
        
        <footer>
            <p>ESP32 ATX电源控制器 © 2025</p>
        </footer>
    </div>
</body>
</html>
)";

static esp_err_t http_handler_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    
    // 发送HTML头部
    httpd_resp_sendstr_chunk(req, s_root_html_head);
    
    // 动态添加各模块注册的链接
    char link_entry[512];
    for (size_t i = 0; i < s_link_count; i++) {
        if (s_module_links[i].name && s_module_links[i].url) {
            const char* description = "";
            if (strcmp(s_module_links[i].name, "ATX电源控制") == 0) {
                description = "控制电脑电源开关、查看电源状态";
            } else if (strcmp(s_module_links[i].name, "OTA固件升级") == 0) {
                description = "在线升级固件版本";
            }
            
            snprintf(link_entry, sizeof(link_entry), 
                "                <div class=\"module-card\">\n"
                "                    <a href=\"%s\">\n"
                "                        <div class=\"module-title\">%s</div>\n"
                "                        <div class=\"module-description\">%s</div>\n"
                "                    </a>\n"
                "                </div>\n", 
                s_module_links[i].url, s_module_links[i].name, description);
            httpd_resp_sendstr_chunk(req, link_entry);
        }
    }
    
    // 发送HTML尾部
    httpd_resp_sendstr_chunk(req, s_root_html_tail);
    httpd_resp_sendstr_chunk(req, NULL); // 结束响应
    return ESP_OK;
}

// Favicon处理
static esp_err_t http_handler_favicon(httpd_req_t *req) {
    // 返回一个简单的16x16 PNG图标数据
    const char favicon_png[] = {
        "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A\x00\x00\x00\x0D\x49\x48\x44\x52"
        "\x00\x00\x00\x10\x00\x00\x00\x10\x08\x06\x00\x00\x00\x1F\xF3\xFF"
        "\x61\x00\x00\x00\x4A\x49\x44\x41\x54\x78\xDA\x63\xFC\xFF\xFF\x3F"
        "\x03\x31\x80\x89\x81\x88\x06\x30\x31\x90\x62\x00\x0B\x91\x06\x30"
        "\x91\x6A\x00\x33\x91\x06\x30\x93\x68\x00\x0B\xA9\x06\xb0\x50\x64"
        "\x00\x0B\xB9\x06\xb0\x50\x6C\x00\x0B\xD5\x06\xb0\xd0\x6D\x00\x0B"
        "\xDD\x06\xb0\x30\x6C\x00\x0B\xD3\x06\xb0\xb0\x6D\x00\x0B\xDF\x06"
        "\x30\x88\x18\xc0\x20\x65\x00\x83\x9C\x01\x0C\x8A\x06\x30\x28\x1B"
        "\xC0\xa0\x6E\x00\x83\xC6\x01\x0C\x2A\x07\x30\xe8\x1C\xc0\x60\x74"
        "\x00\x83\xD9\x01\x0C\x76\x07\x30\x38\x1D\xc0\xe0\x75\x00\x83\xDF"
        "\x01\x0C\x8E\x07\x30\x78\x1E\xc0\xe0\x7A\x00\x83\xEF\x01\x0C\xCE"
        "\x07\x30\xf8\x1e\xc0\x10\x7C\x00\x43\xF4\x01\x0C\xD1\x07\x30\x44"
        "\x1F\xc0\x10\x7D\x00\x43\xF4\x01\x0C\xD1\x07\x30\x44\x1F\xc0\x10"
        "\x7D\x00\x43\xF4\x01\x0C\xD1\x07\x30\x44\x1F\xc0\x90\x7D\x00\x43"
        "\xF6\x01\x0C\xD9\x07\x30\x64\x1F\xc0\x90\x7D\x00\x43\xF6\x01\x0C"
        "\xD9\x07\x30\x64\x1F\xc0\x90\x7D\x00\x43\xF6\x01\x0C\xD9\x07\x30"
        "\x00\x00\x00\x00\xFF\xFF\xFF\xFF\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x49\x45\x4E\x44\xAE\x42\x60\x82"
    };
    
    httpd_resp_set_type(req, "image/png");
    httpd_resp_send(req, favicon_png, sizeof(favicon_png)-1);
    return ESP_OK;
}

// 基础路由（只有根路径）
static const HttpRoute_t s_base_routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = http_handler_root, .user_ctx = NULL},
    {.uri = "/favicon.png", .method = HTTP_GET, .handler = http_handler_favicon, .user_ctx = NULL},
};

esp_err_t http_server_register_routes(const HttpRoute_t *routes, size_t route_count) {
    if (!s_httpd_handle || !routes || route_count == 0) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < route_count; i++) {
        httpd_uri_t uri_handler = {
            .uri       = routes[i].uri,
            .method    = routes[i].method,
            .handler   = routes[i].handler,
            .user_ctx  = routes[i].user_ctx
        };
        httpd_register_uri_handler(s_httpd_handle, &uri_handler);
        ESP_LOGD(TAG, "注册路由：%s %s",
                 routes[i].method == HTTP_GET ? "GET" : "POST", routes[i].uri);
    }
    return ESP_OK;
}

esp_err_t http_server_init(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.stack_size = 16384;
    config.max_open_sockets = 5;
    config.max_uri_handlers = 16;

    if (httpd_start(&s_httpd_handle, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP服务器启动失败");
        return ESP_FAIL;
    }
    // 注册基础路由
    http_server_register_routes(s_base_routes, sizeof(s_base_routes)/sizeof(HttpRoute_t));
    ESP_LOGI(TAG, "HTTP服务器启动成功，端口：%d", HTTP_SERVER_PORT);

    return ESP_OK;
}


esp_err_t http_server_register_module_link(const char *name, const char *url) {
    if (!name || !url || s_link_count >= sizeof(s_module_links)/sizeof(s_module_links[0])) {
        return ESP_ERR_NO_MEM;
    }
    
    s_module_links[s_link_count].name = name;
    s_module_links[s_link_count].url = url;
    s_link_count++;
    
    ESP_LOGI(TAG, "注册模块链接: %s -> %s", name, url);
    return ESP_OK;
}