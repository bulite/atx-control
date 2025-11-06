#include "common.h"
#include "esp_timer.h"
#include "string.h"
#include "ctype.h"

// 日志环形缓冲区
static char s_log_buffer[LOG_BUFFER_SIZE];
static volatile size_t s_log_write_idx = 0;
static volatile size_t s_log_read_idx = 0;
static portMUX_TYPE s_log_mutex = LOG_MUTEX;

uint64_t common_get_time_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000;
}
static int log_redirect_cb(const char *fmt, va_list ap) {
    if (!s_log_mutex) {
        return vprintf(fmt, ap);  // 互斥锁未初始化时，输出到串口
    }

    char log_buf[256];  // 单条日志缓冲区
    int len = vsnprintf(log_buf, sizeof(log_buf), fmt, ap);  // 格式化日志
    if (len <= 0 || len >= sizeof(log_buf)) {
        return len;
    }

    // 将日志写入循环缓冲区（线程安全）
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    for (int i = 0; i < len; i++) {
        s_log_buffer[s_log_write_idx] = log_buf[i];
        s_log_write_idx = (s_log_write_idx + 1) % LOG_BUFFER_SIZE;
        // 避免写指针追上读指针（循环覆盖旧日志）
        if (s_log_write_idx == s_log_read_idx) {
            s_log_read_idx = (s_log_read_idx + 1) % LOG_BUFFER_SIZE;
        }
    }
    xSemaphoreGive(s_log_mutex);

    return len;  // 返回实际写入的字符数
}

void common_log_buffer_write(const char *data, size_t len) {
    if (len == 0 || data == NULL) return;
    // 仅存储I/W/E级日志
    if (data[0] != 'I' && data[0] != 'W' && data[0] != 'E') return;

    portENTER_CRITICAL(&s_log_mutex);
    for (size_t i = 0; i < len; i++) {
        s_log_buffer[(s_log_write_idx + i) % LOG_BUFFER_SIZE] = data[i];
    }
    s_log_write_idx = (s_log_write_idx + len) % LOG_BUFFER_SIZE;
    portEXIT_CRITICAL(&s_log_mutex);
}

char* common_log_buffer_read(size_t *read_len) {
    static char read_buf[1024];
    *read_len = 0;

    portENTER_CRITICAL(&s_log_mutex);
    if (s_log_write_idx >= s_log_read_idx) {
        *read_len = s_log_write_idx - s_log_read_idx;
    } else {
        *read_len = LOG_BUFFER_SIZE - s_log_read_idx;
    }
    *read_len = (*read_len > sizeof(read_buf)-1) ? sizeof(read_buf)-1 : *read_len;

    if (*read_len > 0) {
        memcpy(read_buf, &s_log_buffer[s_log_read_idx], *read_len);
        s_log_read_idx = (s_log_read_idx + *read_len) % LOG_BUFFER_SIZE;
        read_buf[*read_len] = '\0';
    }
    portEXIT_CRITICAL(&s_log_mutex);

    return read_buf;
}

bool common_base64_decode(const char *in, size_t in_len, char *out, size_t out_len) {
    static const unsigned char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char dtable[256];
    memset(dtable, 0x80, 256);
    for (int i = 0; i < 64; i++) dtable[base64_table[i]] = i;
    dtable['='] = 0;

    int bits = 0, val = 0;
    size_t out_idx = 0;
    for (size_t i = 0; i < in_len && in[i] != '='; i++) {
        if (dtable[(unsigned char)in[i]] == 0x80) return false;
        val = (val << 6) | dtable[(unsigned char)in[i]];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_idx >= out_len) return false;
            out[out_idx++] = (val >> bits) & 0xFF;
        }
    }
    out[out_idx] = '\0';
    return true;
}

bool common_md5_verify(const uint8_t *data, size_t len, const char *expected_md5) {
    md5_context_t ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    uint8_t result[16];
    md5_final(&ctx, result);

    char result_str[33] = {0};
    for (int i = 0; i < 16; i++) {
        snprintf(&result_str[i*2], 3, "%02x", result[i]);
    }
    return strcasecmp(result_str, expected_md5) == 0;
}

esp_err_t common_log_init(void) {
    s_log_mutex = xSemaphoreCreateMutex();
    esp_log_set_vprintf(log_redirect_cb);
    return ESP_OK;
}
