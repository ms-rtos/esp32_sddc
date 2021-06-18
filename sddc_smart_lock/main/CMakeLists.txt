idf_component_register(SRCS "sddc_esp32_smart_lock.c" "esp32_connect.c" "sddc.c" "camera/bitmap.c" "camera/camera.c" "camera/ov2640.c" "camera/ov7725.c" "camera/sccb.c" "camera/twi.c" "camera/wiring.c" "camera/xclk.c"
                    INCLUDE_DIRS "." "camera" "camera/include"
                    PRIV_REQUIRES esp_netif nvs_flash json
                    )
