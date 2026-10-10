# esp_websocket_client

本工程随源码提供 Espressif 的 WebSocket 客户端 **1.2.3**，由 PlatformIO
作为本地库编译，无需复制全局 SDK 中的头文件或静态库。

上游：https://github.com/espressif/esp-protocols/tree/websocket-v1.2.3/components/esp_websocket_client

固定提交：`1750a3fda9c99a9545379f89c6bb49a86615cfaa`。

`src/esp_websocket_client.c`、`src/esp_websocket_client.h` 和 `LICENSE`
来自该提交，源码未修改，仅调整文件位置以适配 PlatformIO 的库目录结构。
许可证为 Apache-2.0，完整文本见 `LICENSE`。

组件要求 ESP-IDF >= 5.0。本项目固定的 Arduino-ESP32 3.0.7 SDK 提供
`tcp_transport`、`esp-tls`、`http_parser`、`esp_event`、`esp_timer` 和 lwIP
依赖；保留上游默认的静态收发缓冲区设置。
