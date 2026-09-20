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
| 端到端链路 | ⬜ 只缺真凭据（`src/Secrets.h` + `src/ServerCaCert.h`） |

## 文档入口

-   **远程访问功能全貌**（服务器端 + 单片机端）→ [`docs/REMOTE_ACCESS.md`](docs/REMOTE_ACCESS.md)
-   **远程监控使用指南**（怎么看数据、设备怎么上线、怎么排障）→ [`docs/REMOTE_ACCESS_GUIDE.md`](docs/REMOTE_ACCESS_GUIDE.md)
-   服务器端部署与 API → [`server/README.md`](server/README.md)
-   AI 编码代理操作约束 → [`AGENTS.md`](AGENTS.md)

## 协作者接入

> 本仓库是 **private**，`docs/` 下含服务器 IP、Nginx 布局与部署脚本。
> **加人 = 交出全部提交历史与这些情报**，而且对方已 clone 的副本无法收回。加之前先想清楚。

### 1. 加人（仓库所有者操作）

GitHub → 仓库 **Settings → Collaborators → Add people** → 填用户名或邮箱 → 权限选 **Write** → 对方接受邮件邀请后生效。

⚠️ 免费档的 private 仓库**不支持分支保护**（需 GitHub Pro / Team），所以“直推 master”没有技术闸门。
请到 Settings → Branches 自行确认当前档位；若确实没有保护，下面第 5 节的约定就是唯一防线。

### 2. 对方需要准备

| 项 | 说明 |
|---|---|
| Git + VS Code | 仓库带 `.vscode/extensions.json`，打开时会提示安装 PlatformIO IDE 扩展 |
| clone | `git clone https://github.com/emila58035/Cell_incubator.git` |
| **首次构建需联 GitHub** | `platformio.ini` 的 `platform_packages` 从 GitHub 拉取钉住的 `arduino-esp32 3.0.7` 与 `esp32-arduino-libs`。中国大陆直连 `github.com:443` 常常超时（本项目开发机就是如此），需自备可用网络 |
| 构建 | `pio run`（默认 env = 生产固件）；`pio run -e sim` = 无传感器模拟构建 |
| 行尾 | `.gitattributes` 强制 LF / UTF-8 无 BOM，**不要**去改 `core.autocrlf` |

### 3. clone 之后拿不到的三个文件（必须带外传）

它们被 `.gitignore` 排除，**不会**随仓库到达；仓库里只有 `.example` 模板。缺了它们 `pio run` 依然成功，但设备不会联网。

| 文件 | 内容 | 怎么给 |
|---|---|---|
| `src/Secrets.h` | Wi-Fi SSID/密码、服务器 URL、设备 ID、设备 Token | 复制 `src/Secrets.h.example` 填值；**值由所有者通过仓库外渠道单独发**，不要写进 issue / PR / 提交消息 |
| `src/ServerCaCert.h` | 自签 CA 的**公钥**证书 | 取服务器上的 `incubator-ca.pem`（位置见 `docs/REMOTE_ACCESS.md` §2.4）；**私钥永不出服务器** |
| 看板 Basic Auth 密码 | 浏览器看数据用 | 所有者单独给；重置方法见 `docs/REMOTE_ACCESS.md` §4.9 |

设备 Token 存于服务器，格式与权限见 `docs/REMOTE_ACCESS.md` §2.4。旧版的 `secrets.ini` 已退役。

### 4. 首次烧录必读

- **分区表已从 4 MB 换成 8 MB**：首次烧录前**必须擦除整片 flash**，否则残留的 `otadata` 会指向旧槽位。
- **`pio run -e sim` 绠不能烧到接了加热/阀门的板子上** —— 模拟器喂的是合成温度，PID 会据此驱动**真实**输出。sim 固件版本号是 `0.1.0-sim`，便于事后区分数据。
- 日志走 UART0 → CH340K，**不是**原生 USB（原因与端口见下面「给接手者」）。
- 烧录前可用只读探测定身份：`platformio pkg exec --package tool-esptoolpy -- esptool.py --port <COM口> flash_id`。

### 5. 协作约定

- **一个提交一个主题**；动了 `src/` 必须 `pio run` 通过再提交（改过 `src/` 建议两个 env 都编）。
- 结构规则见 [`AGENTS.md`](AGENTS.md)。若你用 AI 编码工具，它会自动加载该文件——里面的判级与禁令**同样适用于你**。
- 服务器 IP / 域名 / Nginx 路径**只允许出现在 `docs/`**；写进 `src/` 或顶层文件即视为错误。
- 凭据永不入库、永不出现在提交消息或 issue 里。
- **不 force-push、不改写已推送的历史**；确需改写先说明并取得同意。

### 6. 若同时给了服务器 SSH

那台机器**不是本项目专用机**：上面跑着与本项目无关的长期服务，包括一个大体积的游戏服务器目录、面板与 VNC。
**动手前必读** `docs/REMOTE_ACCESS.md` §2.5「机器上的既有服务与红线」：那里列了不得触碰的路径、
不得使用的命令（包括会连带破坏既有 OTA 站点的证书工具用法）、面板配置为何不生效、
以及磁盘紧张时的正确处理方式。数据库是唯一有状态的资产，改前先备份。

### 7. 移除协作者

Settings → Collaborators → 移除。注意：**对方已 clone 的副本与已拉取的历史无法收回**。
若怀疑凭据外泄，**轮换设备 Token 与看板密码比移除权限更紧迫**（方法见 `docs/REMOTE_ACCESS.md` §4.9）。

## 给接手者

-   仓库是 **private**（文档含服务器 IP 与 Nginx 布局），不得转公开。
-   全部 LF / UTF-8 无 BOM，由 `.gitattributes` 强制。
-   本机 `github.com:443` 直连超时。遇到网络故障**不要自行配代理**，先向使用者说明现象并问怎么处理。
-   改完代码跑 `pio run`，没通过不要提交。
-   显示配置在 `src/User_Setup.h`（ST7789 240×320，MOSI=21/SCLK=40/CS=16/DC=17/BL=15），由 `platformio.ini` 的 `build_flags` 强制加载（`-DUSER_SETUP_LOADED=1` + `-include`）。换面板要同时改 `src/DisplayManager.h` 里的 `#error` 防线。
-   联网凭据走 **`src/Secrets.h`**（gitignored，模板 `src/Secrets.h.example`）+ **`src/ServerCaCert.h`**（模板 `.example` 同目录），两者由 `NetworkConfig.h` 用 `__has_include` 条件包含，**缺失也能编译**（回落为空 → 只打印一次并停止上报）。旧的 `secrets.ini` + `-D` 注入已退役：PlatformIO 的 build_flags 解析器无法携带空格，SSID 带空格就会编译失败（详见 `platformio.ini` 里的注释与 `docs/REMOTE_ACCESS.md` §6.4）。
-   日志走 **UART0 → CH340K（COM22）**：`platformio.ini` 用 `-DARDUINO_USB_CDC_ON_BOOT=0` + `build_unflags` 覆盖了 board def 的 `=1`，并关掉了 board def 的 1200bps-touch 上传流程（后者会让 PlatformIO 换到原生 USB 口然后配置失败）。烧录与监视：`pio run -t upload --upload-port COM22`、`pio device monitor -p COM22`（`monitor_speed = 115200` 已在 ini 里）。原生 USB 那条会在复位时换号并丢掉主机打开前的输出，不要用它看日志。
-   实物是 **ESP32-S3 裸板**（8 MB flash、8 MB 八线 PSRAM、CH340K + 原生 USB 双通道）。`src/Secrets.h` 缺失时固件不会重试联网，只打一行 `[WIFI] No SSID configured`。
-   烧录前可用只读探测定身份：`platformio pkg exec --package tool-esptoolpy -- esptool.py --port COM22 flash_id`。
-   **无传感器开发**用 `pio run -e sim`（详见 `docs/REMOTE_ACCESS.md` §6.9）：模拟器只在 `SystemState` 写入层顶替驱动，PID/阀门/显示/payload 都是真的；payload 每周期打到串口，无 AP 也能验契约。⚠️ **绝不能烧到接着执行器的板子上**（假温度会驱动真实 PWM）。模拟构建报 `firmware_version = 0.1.0-sim`，便于事后区分数据。
-   动过 `src/` 后建议两个 env 都编：`pio run` 与 `pio run -e sim`（生产构建用 `build_src_filter` 排除了 `SensorSimulator.cpp`，只编生产不会发现模拟器编译错误）。
