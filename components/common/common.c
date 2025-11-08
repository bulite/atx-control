#include "string.h"
#include "ctype.h"
#include "mbedtls/md5.h"
#include "common.h"
#include "esp_timer.h"
#include "http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"

// 日志环形缓冲区
static char s_log_buffer[LOG_BUFFER_SIZE];
static volatile size_t s_log_write_idx = 0;
static volatile size_t s_log_read_idx = 0;
static SemaphoreHandle_t s_log_mutex = NULL;

static stateCallback_t s_state_callbacks[5] = {NULL}; // 支持最多5个模块状态回调
static size_t s_callback_count = 0;
uint64_t common_get_time_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static int log_redirect_cb(const char *fmt, va_list ap)
{
    if (s_log_mutex == NULL)
    {
        return vprintf(fmt, ap); // 互斥锁未初始化时，输出到串口
    }

    char log_buf[256];                                      // 单条日志缓冲区
    int len = vsnprintf(log_buf, sizeof(log_buf), fmt, ap); // 格式化日志
    if (len <= 0 || len >= sizeof(log_buf))
    {
        return len;
    }

    printf("%s", log_buf);

    // 将日志写入循环缓冲区（线程安全）
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    for (int i = 0; i < len; i++)
    {
        s_log_buffer[s_log_write_idx] = log_buf[i];
        s_log_write_idx = (s_log_write_idx + 1) % LOG_BUFFER_SIZE;
        // 避免写指针追上读指针（循环覆盖旧日志）
        if (s_log_write_idx == s_log_read_idx)
        {
            s_log_read_idx = (s_log_read_idx + 1) % LOG_BUFFER_SIZE;
        }
    }
    xSemaphoreGive(s_log_mutex);

    return len; // 返回实际写入的字符数
}

void common_log_buffer_write(const char *data, size_t len)
{
    if (len == 0 || data == NULL)
        return;
    // 仅存储I/W/E级日志
    if (data[0] != 'I' && data[0] != 'W' && data[0] != 'E')
        return;

    portENTER_CRITICAL(&s_log_mutex);
    for (size_t i = 0; i < len; i++)
    {
        s_log_buffer[(s_log_write_idx + i) % LOG_BUFFER_SIZE] = data[i];
    }
    s_log_write_idx = (s_log_write_idx + len) % LOG_BUFFER_SIZE;
    portEXIT_CRITICAL(&s_log_mutex);
}

// 新增函数：读取全部日志内容（不更新读取索引）
char *common_log_buffer_read(size_t *read_len)
{
    static char read_buf[LOG_BUFFER_SIZE];
    *read_len = 0;

    portENTER_CRITICAL(&s_log_mutex);
    if (s_log_write_idx >= s_log_read_idx)
    {
        // 数据是连续的
        *read_len = s_log_write_idx - s_log_read_idx;
        if (*read_len > 0)
        {
            memcpy(read_buf, &s_log_buffer[s_log_read_idx], *read_len);
        }
    }
    else if (s_log_write_idx < s_log_read_idx)
    {
        // 数据环绕了缓冲区
        size_t first_part_len = LOG_BUFFER_SIZE - s_log_read_idx;
        memcpy(read_buf, &s_log_buffer[s_log_read_idx], first_part_len);
        memcpy(read_buf + first_part_len, &s_log_buffer[0], s_log_write_idx);
        *read_len = first_part_len + s_log_write_idx;
    }
    read_buf[*read_len] = '\0';
    portEXIT_CRITICAL(&s_log_mutex);

    return read_buf;
}

bool common_base64_decode(const char *in, size_t in_len, char *out, size_t out_len)
{
    static const unsigned char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char dtable[256];
    memset(dtable, 0x80, 256);
    for (int i = 0; i < 64; i++)
        dtable[base64_table[i]] = i;
    dtable['='] = 0;

    int bits = 0, val = 0;
    size_t out_idx = 0;
    for (size_t i = 0; i < in_len && in[i] != '='; i++)
    {
        if (dtable[(unsigned char)in[i]] == 0x80)
            return false;
        val = (val << 6) | dtable[(unsigned char)in[i]];
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (out_idx >= out_len)
                return false;
            out[out_idx++] = (val >> bits) & 0xFF;
        }
    }
    out[out_idx] = '\0';
    return true;
}

bool common_md5_verify(const uint8_t *data, size_t len, const char *expected_md5)
{
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_update(&ctx, data, len);
    uint8_t result[16];
    mbedtls_md5_finish(&ctx, result);

    char result_str[33] = {0};
    for (int i = 0; i < 16; i++)
    {
        snprintf(&result_str[i * 2], 3, "%02x", result[i]);
    }
    return strcasecmp(result_str, expected_md5) == 0;
}

// 日志监控处理
esp_err_t common_http_handler_log(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    // HTML页面头部，包含自动刷新脚本
    const char log_page_head[] = R"(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta http-equiv="refresh" content="2">
    <title>ATX电源控制器 - 实时日志</title>
    <style>
        body { font-family: monospace; background-color: #000; color: #0f0; padding: 10px; }
        pre { white-space: pre-wrap; word-wrap: break-word; margin: 0; }
        .back-link { margin-bottom: 10px; display: block; color: #0ff; text-decoration: none; }
        .back-link:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <a href="/" class="back-link">&lt;&lt; 返回主页</a>
    <h2>实时日志监控</h2>
    <pre>
)";

    httpd_resp_sendstr_chunk(req, log_page_head);

    // 获取并发送日志内容
    size_t read_len = 0;
    char *log_data = common_log_buffer_read(&read_len);
    if (read_len > 0)
    {
        httpd_resp_send_chunk(req, log_data, read_len);
    }

    // HTML页面尾部
    const char log_page_tail[] = R"(
    </pre>
</body>
</html>
)";

    httpd_resp_sendstr_chunk(req, log_page_tail);
    httpd_resp_sendstr_chunk(req, NULL); // 结束响应

    return ESP_OK;
}

esp_err_t common_register_state_callback(stateCallback_t callback)
{
    if (!callback || s_callback_count >= sizeof(s_state_callbacks) / sizeof(s_state_callbacks[0]))
    {
        return ESP_ERR_NO_MEM;
    }
    s_state_callbacks[s_callback_count++] = callback;
    return ESP_OK;
}

// 系统状态聚合处理
char *common_get_system_state(void)
{
    char *state_buf = malloc(2048);
    if (!state_buf)
        return NULL;

    strcpy(state_buf, "{\n");

    // 调用各模块状态回调
    for (size_t i = 0; i < s_callback_count; i++)
    {
        if (s_state_callbacks[i])
        {
            char *module_state = s_state_callbacks[i]();
            if (module_state)
            {
                strcat(state_buf, "  ");
                strcat(state_buf, module_state);
                free(module_state);
                strcat(state_buf, ",\n");
            }
        }
    }

    // 移除最后一个逗号
    if (strlen(state_buf) > 2 && state_buf[strlen(state_buf) - 2] == ',')
    {
        state_buf[strlen(state_buf) - 2] = '\n';
    }
    strcat(state_buf, "}");

    return state_buf;
}

// 系统状态处理
esp_err_t common_http_handler_state(httpd_req_t *req)
{
    char *state_buf = common_get_system_state();
    if (!state_buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存不足");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_send(req, state_buf, strlen(state_buf));
    free(state_buf);
    return ESP_OK;
}

// 日志和状态路由
static const HttpRoute_t s_common_routes[] = {
    {.uri = "/log", .method = HTTP_GET, .handler = common_http_handler_log, .user_ctx = NULL},
    {.uri = "/state", .method = HTTP_GET, .handler = common_http_handler_state, .user_ctx = NULL},
};

esp_err_t common_init(void)
{

    s_log_mutex = xSemaphoreCreateMutex();
    esp_log_set_vprintf(log_redirect_cb);

    // 注册路由
    esp_err_t ret = http_server_register_routes(s_common_routes, sizeof(s_common_routes) / sizeof(HttpRoute_t));
    if (ret != ESP_OK)
        return ret;

    // 注册模块链接
    ret = http_server_register_module_link("实时日志监控", "/log");
    if (ret != ESP_OK)
        return ret;

    // ret = http_server_register_module_link("系统状态查询", "/state");
    // if (ret != ESP_OK)
    //     return ret;

    return ESP_OK;
}