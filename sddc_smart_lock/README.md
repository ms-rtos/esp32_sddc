
# ESP32 SDDC Smart Lock Demo

EdgerOS SDDC Smart Lock Demo for ESP32.

## Hardware Required

This example can be run on any commonly available ESP32 development board.

## Configure the project

```
idf.py menuconfig
```

Set following parameters under Example Connnection Configuration Options:

* Set `WiFi SSID` of the Spirit (Access-Point).

* Set `WiFi Password` of the Spirit (Access-Point).

## Build and Flash

Build the project and flash it to the board, then run monitor tool to view serial output:

```
idf.py build
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the Getting Started Guide for full steps to configure and use ESP-IDF to build projects.

