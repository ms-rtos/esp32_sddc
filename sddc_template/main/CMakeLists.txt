idf_component_register(SRCS "sddc_esp32_example.c" "esp32_connect.c" "sddc.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_netif nvs_flash json
                    )
