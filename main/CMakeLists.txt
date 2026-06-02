#[[
file(GLOB_RECURSE SOURCES "src/*.c")

idf_component_register(
    SRCS "src/blink_test.c", ${SOURCES}
    INCLUDE_DIRS "include"
)
]]

idf_component_register(SRCS "blink_test.c"
                            "src/verify_test.c"
                            "src/uart_modbus.c"
                            "src/enc28j60.c"
                            "src/wifi_mqtt.c"
                            "src/ota.c"
                            "src/config_portal.c"
                        INCLUDE_DIRS "include"
                        REQUIRES driver esp_netif esp_wifi mqtt esp_event nvs_flash efuse esp_https_ota esp_http_server
                        )