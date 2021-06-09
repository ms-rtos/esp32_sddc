
# esp32_sddc

[ESP32](https://www.espressif.com/zh-hans/products/socs/esp32) 是乐鑫科技推出的一款面向物联网应用的高性能低功耗的高性价比、高度集成的 Wi-Fi & 蓝牙 MCU。

乐鑫科技对外开放了 ESP32 的二次开发 SDK [ESP32-IDF](https://github.com/espressif/esp-idf)，并提供了该 SDK 的 [编程指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/index.html)

翼辉信息工程师基于 `ESP32-IDF` SDK 上移植了 [libsddc](https://github.com/ms-rtos/libsddc) ，实现了 ESP32 与搭载 EdgerOS 的智能边缘计算机 Spirit1 通信。

`esp32_sddc` 是 libsddc 在 `ESP32-IDF` 上的移植工程，里面包含了 `libsddc` 工程模板 `sddc_template` 和一些示例工程，如 `sddc_camera`。

`esp32_sddc` 的使用请参考 [ESP32 SDDC 设备开发](https://www.edgeros.com/ms-rtos/guide/esp32_sddc_develop.html)

`esp32_sddc` 使用 Apache License 2.0 开源协议。