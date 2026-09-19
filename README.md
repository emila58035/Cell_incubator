# Cell Incubator — 细胞培养箱控制器

> ESP32-S3 固件 + 服务器端远程监控。本地温控已就绪，联网代码已烧录上板（待填真凭据联调）。

## 是什么

细胞培养箱的嵌入式控制系统。硬件是 ESP32-S3R8 核心板（8 MB flash + 8 MB PSRAM，双 USB 通道），软件栈 PlatformIO / Arduino。

> ⚠️ `platformio.ini` 仍沿用 `board = adafruit_feather_esp32s3`，与实物不完全一致（Adafruit 的 `variant` 与 `hwids`、`maximum_size` 4 MB）。**已按实测改掉的**：8 MB 分区表、`flash_size = 8MB`、日志通道回 UART0、上传不跑 1200bps-touch。**仍待决策**：八线 PSRAM 未启用（`memory_type`）、board 定义是否脱离 Feather。事实与证据见 `docs/REMOTE_ACCESS.md` §6.8。

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

`pio run` 结果：RAM **17.9%** / Flash **53.6%**（app 槽 2048 KB，已切 8 MB 分区表；未接入联网模块时为 12.1% / 33.1%）。

## 仓库布局

```
src/             固件（12 个模块 + main.cpp + 配置头 Config.h / NetworkConfig.h / User_Setup.h）
server/          服务器端：FastAPI + SQLite + Nginx 反代（已上线，见 server/README.md）
docs/            功能文档
platformio.ini   env: adafruit_feather_esp32s3
AGENTS.md        AI 编码代理的结构维护规则（常驻注入）
```

## 当前进度

| 部分 | 状态 |
|---|---|
| 本地温控固件 | ✅ 已上板运行（启动日志正常；执行器未接，加热自动暂停） |
| 服务器端 | ✅ 已上线（HTTPS + Basic Auth + SQLite，`systemd` 守护） |
| 看板 | ✅ 浏览器可访问，已人工验收 |
| 单片机联网 | ✅ 已编码 + 已烧录（启动日志：`netTask on core 0`、`internal=1`、30 s 无重启；**未联调**） |
| 模拟数据源 | ✅ `pio run -e sim` 可用（8 阶段故障注入；payload 经服务器模型离线校验 **0 invalid**） |
| 端到端链路 | ⬜ 只缺真凭据（`secrets.ini` + `src/ServerCaCert.h`） |

## 文档入口

-   **远程访问功能全貌**（服务器端 + 单片机端）→ [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md)
-   **远程监控使用指南**（怎么看数据、设备怎么上线、怎么排障）→ [`docs/REMOTE_ACCESS_GUIDE.md`](docs/REMOTE_ACCESS_GUIDE.md)
-   服务器端部署与 API → [`server/README.md`](server/README.md)
-   AI 编码代理操作约束 → [`AGENTS.md`](AGENTS.md)

## 给接手者

-   仓库是 **private**（文档含服务器 IP 与 Nginx 布局），不得转公开。
-   全部 LF / UTF-8 无 BOM，由 `.gitattributes` 强制。
-   本机 `github.com:443` 直连超时。遇到网络故障**不要自行配代理**，先向使用者说明现象并问怎么处理。
-   改完代码跑 `pio run`，没通过不要提交。
-   显示配置在 `src/User_Setup.h`（ST7789 240×320，MOSI=21/SCLK=40/CS=16/DC=17/BL=15），由 `platformio.ini` 的 `build_flags` 强制加载（`-DUSER_SETUP_LOADED=1` + `-include`）。换面板要同时改 `src/DisplayManager.h` 里的 `#error` 防线。
-   联网凭据走 `secrets.ini`（`extra_configs`，**必须写在 `[platformio]` 段**）+ `src/ServerCaCert.h`，两者都有 `.example` 模板且缺失时可编译（回落为空 → 只打印一次并停止上报）。
-   日志走 **UART0 → CH340K（COM22）**：`platformio.ini` 用 `-DARDUINO_USB_CDC_ON_BOOT=0` + `build_unflags` 覆盖了 board def 的 `=1`，并关掉了 board def 的 1200bps-touch 上传流程（后者会让 PlatformIO 换到原生 USB 口然后配置失败）。烧录与监视：`pio run -t upload --upload-port COM22`、`pio device monitor -p COM22`（`monitor_speed = 115200` 已在 ini 里）。原生 USB 那条会在复位时换号并丢掉主机打开前的输出，不要用它看日志。
-   实物是 **ESP32-S3 裸板**（8 MB flash、8 MB 八线 PSRAM、CH340K + 原生 USB 双通道）。`secrets.ini` 缺失时固件不会重试联网，只打一行 `[WIFI] No SSID configured`。
-   烧录前可用只读探测定身份：`platformio pkg exec --package tool-esptoolpy -- esptool.py --port COM22 flash_id`。
-   **无传感器开发**用 `pio run -e sim`（详见 `docs/REMOTE_ACCESS.md` §6.9）：模拟器只在 `SystemState` 写入层顶替驱动，PID/阀门/显示/payload 都是真的；payload 每周期打到串口，无 AP 也能验契约。⚠️ **绝不能烧到接着执行器的板子上**（假温度会驱动真实 PWM）。模拟构建报 `firmware_version = 0.1.0-sim`，便于事后区分数据。
-   动过 `src/` 后建议两个 env 都编：`pio run` 与 `pio run -e sim`（生产构建用 `build_src_filter` 排除了 `SensorSimulator.cpp`，只编生产不会发现模拟器编译错误）。
