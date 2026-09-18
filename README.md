# Cell Incubator — 细胞培养箱控制器

> ESP32-S3 固件 + 服务器端远程监控。本地温控已就绪，联网上报正在施工。

## 是什么

细胞培养箱的嵌入式控制系统。硬件平台 Adafruit Feather ESP32-S3，软件栈 PlatformIO / Arduino。

本地功能闭环：

| 模块 | 说明 |
|---|---|
| 温湿度 | SHT30（I2C，0x44），替换了旧 AHT20 |
| CO₂ | SenseAir S8（UART1 非阻塞轮询） |
| 加热控制 | 双增益 PID（保守 Kp=10 / 激进 Kp=20，切换带 3 ℃），PWM 驱动；默认目标 37 ℃ |
| 阀门 / 风扇 / LED | 各独立 Controller，PWM / GPIO 驱动 |
| 显示 | TFT_eSPI 320×240，三列气候指标布局 |
| 输入 | 旋转编码器 + 按钮，去抖动处理 |
| 设置持久化 | Preferences（NVS），手动保存 |

`pio run` 结果：RAM 12.1% / Flash 33.1%。

## 仓库布局

```
src/             固件（11 个模块 + main.cpp + Config.h）
server/          服务器端：FastAPI + SQLite + Nginx 反代（已上线，见 server/README.md）
docs/            功能文档
platformio.ini   env: adafruit_feather_esp32s3
AGENTS.md        AI 编码代理的结构维护规则（常驻注入）
```

## 当前进度

| 部分 | 状态 |
|---|---|
| 本地温控固件 | ✅ `pio run` SUCCESS（待实机验证） |
| 服务器端 | ✅ 已上线（HTTPS + Basic Auth + SQLite，`systemd` 守护） |
| 看板 | ✅ 浏览器可访问，已人工验收 |
| 单片机联网 | ⬜ 未编码（方案见 `docs/REMOTE_ACCESS.md` §6，**开工前有一个并发模型要定**） |
| 端到端链路 | ⬜ 卡在联网模块 |

## 文档入口

-   **远程访问功能全貌**（服务器端 + 单片机端）→ [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md)
-   服务器端部署与 API → [`server/README.md`](server/README.md)
-   AI 编码代理操作约束 → [`AGENTS.md`](AGENTS.md)

## 给接手者

-   仓库是 **private**（文档含服务器 IP 与 Nginx 布局），不得转公开。
-   全部 LF / UTF-8 无 BOM，由 `.gitattributes` 强制。
-   本机 `github.com:443` 直连超时。遇到网络故障**不要自行配代理**，先向使用者说明现象并问怎么处理。
-   改完代码跑 `pio run`，没通过不要提交。
