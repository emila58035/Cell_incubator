# Cell Incubator — 细胞培养箱控制器

> ESP32-S3 固件（本仓库）+ 服务器端远程监控（`server/`）。本地温控已就绪，联网模块已编码并上板（待填真凭据联调）。

## 文档入口

| 想做什么 | 去看 |
|---|---|
| 了解远程访问的方案、架构与开发进度 | [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md) |
| 日常使用：看数据、设备上线、排障 | [`docs/REMOTE_ACCESS_GUIDE.md`](docs/REMOTE_ACCESS_GUIDE.md) |
| 拿到仓库权限后：拿凭据、构建、烧录、注意事项 | [`docs/ONBOARDING.md`](docs/ONBOARDING.md) |
| 服务器端部署、API 契约与运维 | [`server/README.md`](server/README.md) |
| AI 编码代理的结构维护规则 | [`AGENTS.md`](AGENTS.md) |

## 硬件

实物是一块 **ESP32-S3R8 核心板**（8 MB flash + 8 MB 八线 PSRAM，双 USB 通道），
跑在异常 Adafruit Feather ESP32-S3 的 board 定义下——已按实测改掉 8 MB 分区表、flash_size、日志通道和下载流程，但仍保留 Feather 的 `board =` 与 `variant`。详细口径见 `docs/REMOTE_ACCESS.md` §6.8。

### 引脚

| 功能 | 引脚 | 说明 |
|---|---|---|
| 温湿度传感器 AHT20 | SDA=1, SCL=2, ADDR→0x38 | I2C；SHT20/SHT30 驱动停用在 `src/SHT20Sensor.*`、`src/SHT30Sensor.*` |
| CO₂ 传感器 SenseAir S8 | RX=5, TX=4 (UART1) | 9600 bps，非阻塞轮询 |
| 加热 PWM | GPIO 39 | 2 kHz / 8 bit / 最大 255，不反相 |
| 照明 PWM | GPIO 38 | 1 kHz / 8 bit / 最大 255，不反相 |
| 风扇 | GPIO 47 | 数字输出 |
| 阀门 | GPIO 48 | 数字输出，三段 CO₂ 阈值逻辑 |
| 显示 TFT ST7789 240×320 | MOSI=21, SCLK=40, CS=16, DC=17, BL=15 | 由 `src/User_Setup.h` + `build_flags` 强制加载；实测 sprite DMA 分配成功，面板实际点亮未验证 |
| 旋转编码器 | A=41, B=42, 按钮=45, KO=46 | 去抖动 |
| USB | CH340K (UART0, GPIO 43/44) + 原生 USB | 日志与监视走 CH340K（`COMxx`，每台机器不同）；原生 USB 仅用于固件下载 |

⚠️ **八线 PSRAM 实测不可用**。启动即报 `PSRAM ID read error: 0x00ffffff`，原因是 board 定义链接了 quad 变体 `libesp_psram.a`，修法是加一行 `board_build.arduino.memory_type = qio_opi`，尚未决策。已用 IO 全部避开 PSRAM 占用的 GPIO 33–37。

### 传感器与执行器

| 模块 | 说明 |
|---|---|
| 温湿度 | AHT20（I2C 0x38），典型精度 ±0.3 ℃ / ±2% RH。故障时 `temperatureValid`/`humidityValid` 置 false |
| CO₂ | SenseAir S8，UART1 非阻塞轮询，原值 ×10 存入 `SystemState.co2`（物理单位未确认，服务器不做换算） |
| 加热控制 | 双增益 PID：带内（距目标 < 3 ℃）Kp=10/Ki=0.5，带外 Kp=20/Ki=0。PWM 输出 0–255(ledc 2 kHz)，默认目标 37 ℃。开关仅由用户输入；传感器故障暂停输出但不关闭开关，恢复后自动续控 |
| 阀门 | 三段阈值（25000 / 40000 / 48000，单位同 CO₂），开阀时长分长短两档 |
| 风扇 + 照明 | 各独立控制，照明 PWM 可调亮度 |
| 显示 | TFT_eSPI，三列气候指标布局，精灵图 DMA 缓冲，面板实时脏标记驱动刷新 |
| 输入 | 旋转编码器调节温度/亮度，KO 键切换设置项，按钮确认。去抖动处理 |
| 设置持久化 | Preferences(NVS)，手动保存。magic+version+checksum(FNV-1a) |
| 模拟数据源 | `pio run -e sim`：8 阶段故障注入，合成温度走真实 PID/显示/payload，用于无传感器开发。**绝不能烧到接执行器的板子上** |

## 仓库布局

```
src/             固件——14 个模块 + 4 个配置头（Config / NetworkConfig / User_Setup / Secrets*.example）
server/          服务器端——FastAPI + SQLite + Nginx 反代 + 看板静态页 + 部署脚本（已上线）
docs/            功能文档
platformio.ini   构建配置（env: adafruit_feather_esp32s3 生产 / env:sim 模拟）
AGENTS.md        AI 编码代理的结构维护规则（自动注入会话）
```

## 使用方法

### 构建

```bash
git clone https://github.com/emila58035/Cell_incubator.git
cd Cell_incubator
pio run                 # 生产固件（排除 SensorSimulator.cpp）
pio run -e sim          # 模拟构建（含合成传感器，版本号 0.1.0-sim，绝不能烧到接执行器的板子）
```

构建使用 PlatformIO Core >= 6.1.16，固定 pioarduino `51.03.07`（Arduino-ESP32 **3.0.7** 及配套 SDK/工具链）。WebSocket 客户端 **1.2.3** 源码已包含在 `lib/esp_websocket_client`，无需单独复制头文件或静态库。

打包发送时保留 `platformio.ini`、`src`、`lib` 和根目录分区表 CSV；`.pio` 是构建缓存，无需打包。接收方首次编译需要联网下载依赖，并能访问 GitHub 发布下载地址。

### 凭据文件（联网必需）

两个凭据头文件被 `.gitignore` 排除，clone 后只有 `.example` 模板。缺了它们固件仍能编译（`NetworkConfig.h` 用 `__has_include` 条件包含，回落为空），但不会联网：

1. 复制 `src/Secrets.h.example` → `src/Secrets.h`，填入 Wi-Fi SSID/密码、服务器 URL、设备 ID、设备 Token
2. 复制 `src/ServerCaCert.h.example` → `src/ServerCaCert.h`，粘贴自签 CA 的**公钥**证书（从服务器取 `incubator-ca.pem`，位置见 `docs/REMOTE_ACCESS.md` §2.4；**私钥永不出服务器**）

详细步骤与值的来源见 `docs/ONBOARDING.md` §3。

### 烧录

**首次烧录前必须擦除整片 flash**：分区表已从 4 MB 换成 8 MB，残留的 `otadata` 会指向旧槽位，导致新固件无法启动。

```bash
# 擦除（仅首次）
platformio pkg exec --package tool-esptoolpy -- esptool.py --port <COM口> erase_flash

# 烧录（COM 口号每台机器不同，本机开发机是 COM22）
pio run -t upload --upload-port COM22

# 只读探测（确认板子身份，不擦不写）
platformio pkg exec --package tool-esptoolpy -- esptool.py --port COM22 flash_id
```

### 查看日志

```bash
pio device monitor -p COM22
```

启动信息包括：`[BOOT]` 固件版本与 PSRAM 状态、`[SETTINGS]` NVS 加载结果、`[NET]` netTask 栈地址与 `internal=1`（栈在内部 RAM，未落 PSRAM）。

联网模块缺失凭据时只打印一行 `[WIFI] No SSID configured` 并停止重试——不会阻塞主循环或 PWM 输出。

### 登录看板

浏览器打开服务器的 HTTPS 地址（具体地址见 `docs/REMOTE_ACCESS.md` §2.1），Basic Auth 弹窗输入看板密码即可。看板显示最近一次上报的温湿度/CO₂/加热占空比/设备在线状态；数据为空时说明尚无设备上报——确认设备端的 `src/Secrets.h` 已填且串口日志显示 `[NET]` 上报成功。

看板密码重置方法见 `docs/REMOTE_ACCESS.md` §4.9；设备 Token 的格式与存储位置见同文档 §2.4。

### 网络故障

本机 `github.com:443` 直连超时。**不要自行配代理、改 git 配置或换 remote 协议**——先把现象和报错原文告知项目所有者。

---

## 构建产物参考

| 指标 | 值 |
|---|---|
| RAM 占用 | 58,612 B / 327,680 B（**17.9%**） |
| Flash 占用 | 1,123,477 B / 2,097,152 B（**53.6%**，8 MB 分区表，app 槽 2048 KB） |
| 温控本体（不接联网模块） | RAM 12.1% / Flash 33.1% |
| 凭据为空时 | 编译期折叠掉请求路径会使占用**低估约 35 KB**；真凭据下实际约 **56–57%** |
| `pio run -e sim` | 额外包含 SensorSimulator，构建结果同上 |
