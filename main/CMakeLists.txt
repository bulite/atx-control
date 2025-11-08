idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES wifi_manager http_server common esp_http_server ota_manager atx_controller
)
