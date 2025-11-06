# 组件名称（需与目录名一致）
idf_component_register(
    # 源文件列表
    SRCS "wifi_manager.c"
    # 头文件目录
    INCLUDE_DIRS "include"
    # 依赖的 ESP-IDF 组件
    REQUIRES esp_wifi esp_event nvs_flash esp_netif
)