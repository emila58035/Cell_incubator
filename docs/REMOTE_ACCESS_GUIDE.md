# 远程监控 · 使用指南

> 本指南只回答「**怎么用**」。
> 为什么这样设计、每条结论的实测证据、当前进度与未验证项 → [`REMOTE_ACCESS.md`](REMOTE_ACCESS.md)
> 服务器怎么装、怎么备份、怎么查库 → [`../server/README.md`](../server/README.md)
> 三者冲突时**以代码与实测为准**，并请回报修正。

---

## 1. 一分钟了解它能做什么

| 能做到 | 做不到（当前阶段的设计边界，不是缺陷） |
|---|---|
| 在任意网络位置的手机/电脑上看**实时状态**，并通过 WebSocket 远程修改目标温度、加热、照明亮度、风扇和阀门自动逻辑 | 设备离线时命令不会立即执行；命令有有效期并需要设备回执 |
| **曲线回看**：1 h / 6 h / 24 h / 7 d | 手机推送告警：告警只被记录在看板上，需要主动去看 |
| 看**告警历史**：离线、传感器故障、内部内存偏低 | 断网期间的数据**补传**（丢就丢了，换来的是「能察觉断过网」） |
| 判断数据是否已过期、设备是否离线 | 多设备管理后台（界面按单设备做；协议里已预留 `device_id`） |

一句话：**看板支持实时监控和带回执的远程控制。**

---

## 2. 日常使用：怎么看数据

### 2.1 打开看板

1. 浏览器访问 **`https://175.24.182.190/`**
2. 会提示证书不受信任 —— 这是预期的。证书是**自签的 IP 证书**（本项目不使用域名，原因见 `REMOTE_ACCESS.md` §2.3），选择继续访问即可
3. 弹出登录框，输入**看板用户名/密码**（Basic Auth）。这组密码由服务器安装脚本随机生成、只打印过一次

忘记密码：重置步骤见 `REMOTE_ACCESS.md` §4.9。

### 2.2 看板上有什么、怎么读

| 区域 | 读法 |
|---|---|
| 状态条 | 设备名 + 最后上报时间 + 在线/离线判定 |
| 指标卡 | 温度（含目标值）、湿度、CO₂、加热功率 |
| 执行器 | 加热 / 光照 / 风扇 / 阀门 的当前开关状态 |
| 曲线 | 1 h / 6 h / 24 h / 7 d。**超过 6 h 的视图按 60 s 分桶**，点会变稀 —— 那是聚合，不是丢数据 |
| 告警表 | 见 §2.3 |

上报周期是 **10 s**，「最后上报时间」在 10 s 内跳动属正常。

### 2.3 告警含义

| 告警 | 触发条件 | 意味着什么 | 怎么办 |
|---|---|---|---|
| `offline` | 相邻两次上报间隔 > **30 s**（3 × 上报周期）。由**服务器**判定，不需要有人打开看板 | 设备断电、WiFi 断了，或服务器不可达 | 先查设备供电与 WiFi。恢复后服务器会自动补上 `cleared_ts` |
| `sensor_fault` | 温度/湿度/CO₂ 的「读数有效」标志由 true 变 false | 某个传感器读不到了（接线/探头） | 故障期间加热会**自动暂停**（开关仍显示 ON）—— 先把传感器修好 |
| `low_heap` | 设备内部内存（`heap_int`）低于 **40 KB** | 内存吃紧，可能影响下一次 TLS 上报 | 记下发生时间并反馈；这是为长期无人值守准备的预警 |

### 2.4 三种「看着不对但其实正常」

| 现象 | 原因 |
|---|---|
| 某字段显示 `--` | 本次该字段为**空**（传感器故障时值是 `null`，不是 0）—— 看对应的 `*_ok` 标志 |
| 看板写「尚无设备上报」 | **从来没有**任何样本。检查设备是否上电、凭据是否填好 |
| 数据停在某时刻、状态显示离线 | 设备侧断了（供电/WiFi），或服务器不可达 |

---

## 3. 设备端：上线与日常

### 3.1 首次上线：填两个文件

两个文件都在 `.gitignore` 里（**永不提交**），仓库中各留了一份 `.example` 模板：

| 文件 | 填什么 | 从哪取 |
|---|---|---|
| `src/Secrets.h` | WiFi SSID / 密码、服务器 URL、`device_id`、设备 Token | Token 在服务器的 `/etc/incubator/devices.json` |
| `src/ServerCaCert.h` | 服务器 CA **公钥**证书（整段 PEM） | `/etc/incubator/certs/incubator-ca.pem` |

```bash
# 服务器上，两条只读命令
ssh ubuntu@175.24.182.190
sudo cat /etc/incubator/devices.json            # 取 token
sudo cat /etc/incubator/certs/incubator-ca.pem  # 取 CA 公钥证书
```

四个容易踩的点：

1. **WiFi 必须是 2.4 GHz** —— ESP32-S3 的射频只有 2.4 GHz，5 GHz 的 SSID 它根本看不见。手机热点也行（iPhone 需打开「最大兼容性」）。
2. **`device_id` 必须与 token 归属一致**，否则服务器返回 **403**。
3. 只要 **CA 公钥证书**（`incubator-ca.pem`），不是叶子证书 `server.pem`；**CA 私钥 `incubator-ca.key` 绝不离开服务器**。
4. 这两个头文件是在 `namespace NetworkConfig` **内部**被 include 的 —— **不要**再包一层 namespace（否则报 `NetworkConfig::NetworkConfig::…`）。

烧录：

```bash
pio run -t upload --upload-port COM22          # 生产固件
pio run -e sim -t upload --upload-port COM22   # 开发固件（无传感器时用，见 §5）
```

### 3.2 日常

- **上电即上报**，不需要任何操作。设定值等设置存在 NVS，掉电不丢。
- 生产固件**成功上报时是静默的** —— 串口只在失败时打印。所以「串口没动静」不等于坏了；要确认上报是否成功，看失败行是否存在、或直接看看板。
- **网络故障永远不会影响温控**：所有联网动作都在 Core 0 的独立任务里，控制环在 Core 1，两者只通过微秒级的快照交换数据。断网、URL 写错、服务器宕机，阀门与加热的时序都不变。
- ⚠️ **首次上电或擦除过 flash 时**，设备会以**默认设定值 37 ℃、加热开关 ON** 启动。若温度探头故障，加热会自动暂停，但开关仍显示 ON。

---

## 4. 排障

### 4.1 唯一能看到「设备在想什么」的地方：串口日志

```bash
pio device monitor -p COM22
```

- **COM22** = 板上的 CH340K（接 UART0）。日志与 `Serial.print` 全在这里，115200，已在 `platformio.ini` 里设好。
- 板子的**原生 USB** 那一路只用于 JTAG/调试，**看不到日志**，而且每次复位会换 COM 号 —— 不要用它看日志。
- 端口号可能变（换 USB 口/换电脑）：用 `platformio device list` 确认，或不带 `-p` 让 PlatformIO 自己挑。

### 4.2 日志 → 结论

| 日志 | 含义 | 处理 |
|---|---|---|
| `[BOOT] ESP32-S3 … flash 8388608 B, psram 0 B` | 启动自检：8 MB flash 正常；PSRAM 当前未启用（§7） | 正常 |
| `[WIFI] No SSID configured (src/Secrets.h missing or empty)` | 凭据没生效 | 检查 `src/Secrets.h` 是否存在、是否填了值、是否放在 `src/` 下 |
| `[WIFI] Connecting to "…"` → `Connected, RSSI -38 dBm` | 关联成功 | 正常。RSSI 越接近 0 越好，低于 -80 偏弱 |
| `[WIFI] Link lost` + `Attempt failed; retrying in N ms` | 链路掉了，正在退避重连 | **N 递增（5 s → 10 s → … 60 s 封顶）是设计**，不是卡死 |
| `[NET] Report failed with -1` | 连不上服务器（TCP/TLS 任一步） | 查网络与 URL；服务器侧看 `journalctl -u incubator-api` |
| `HTTP 401` | Token 不对 | 核对 `devices.json` 里的 token |
| `HTTP 403` | `device_id` 与 token 归属不一致 | 两边改成一致 |
| `HTTP 400` | 载荷被服务器校验拒收（字段越界/格式错） | 属固件问题，请反馈 |
| `HTTP 503` | 被限流（20 次/分钟，突发 10） | 正常现象，下一周期会自愈 |
| `Guru Meditation`，或反复出现 `ESP-ROM:`（重启） | 崩溃或看门狗复位 | 把这段串口原文反馈 |

### 4.3 看板侧对不上时

```bash
# 服务器上直接看库（不用打开看板）
sudo -u incubator sqlite3 /var/lib/incubator/data.db \
  "SELECT datetime(recv_ts,'unixepoch','+8 hours') t, temperature, humidity, co2, heater_pwm
   FROM telemetry ORDER BY recv_ts DESC LIMIT 10;"

journalctl -u incubator-api -f          # 服务日志
curl -u <用户名>:<密码> http://127.0.0.1:8000/healthz   # 服务是否活着（服务器本机）
```

---

## 5. 开发/验证：没有传感器也能跑

裸板、没有探头时用**模拟构建**：

```bash
pio run -e sim -t upload --upload-port COM22     # 烧开发固件
pio run -t upload --upload-port COM22            # 回到生产固件
```

- 它**只顶替传感器**：PID、阀门逻辑、显示、上报载荷全部是生产代码。合成数据走 **8 个阶段、400 s 一轮**循环，包含传感器故障、CO₂ 离线/读数不可信、湿度故障等**故障注入**，用来验证看板与告警链路。
- 开发固件报 `firmware_version = 0.1.0-sim`，看板与数据库里一眼可分。清理这些合成数据：
  ```sql
  DELETE FROM telemetry WHERE firmware_version='0.1.0-sim';
  ```
- 它会把**完整的 JSON 载荷**打到串口（链路没连上时也打），所以没有凭据、没有 AP 也能核对契约。
- ⚠️ **绝不要把开发固件烧到接着加热器/阀门的板子上**：合成温度会让 PID 真的输出 PWM、阀门真的脉冲。固件启动时会打印警告。

---

## 6. 凭据与安全

| 事项 | 做法 |
|---|---|
| 凭据放哪 | 只在 `src/Secrets.h` 与 `src/ServerCaCert.h`（都被 git 忽略），仓库里只有 `.example` 模板 |
| 确认没被跟踪 | `git check-ignore -v src/Secrets.h src/ServerCaCert.h` |
| 凭据为什么不用 ini/`-D` | 那条路承载不了含空格的 SSID，八进制转义会被吃掉、`#`/`;` 会被截断（三次实测，见 `REMOTE_ACCESS.md` §6.4） |
| CA 私钥 | `incubator-ca.key` 只留在服务器并离线备份，**永不进固件、永不进仓库** |
| 轮换设备 Token | 编辑服务器的 `/etc/incubator/devices.json`（服务**按 mtime 自动重载，无需重启**）；新 token 用 `openssl rand -hex 32`；改完 `src/Secrets.h` 重烧 |
| 换看板密码 | `REMOTE_ACCESS.md` §4.9 |
| 公网暴露面 | 只有 443；FastAPI 只绑 `127.0.0.1`；`/docs` 已关闭；上报端点 Bearer + 限流（20 r/m，body ≤ 4 KB） |
| 服务器上还有别的服务 | 本项目只新增一个 Nginx 配置文件。**不要动 80 端口的 OTA、VNC 与面板**（见 `REMOTE_ACCESS.md` §2.1） |

---

## 7. 已知限制与未验证项

照实列出，别当承诺：

| 项 | 现状 |
|---|---|
| **CO₂ 单位未确认** | 上报值与设备屏幕一致，但**物理单位取决于 S8 的量程**（固件里有 `Co2ScaleFactor = 10`）。因此**服务器不做任何 CO₂ 阈值告警**，看板也不做换算 |
| **三处超时值未实测** | 连接 3 s / 握手 8 s / 响应 3 s 目前只有**源码审查**证据。它们不影响温控（§3.2），但决定「故障时设备看起来死多久」 |
| **PSRAM 未启用** | 模块带 8 MB 八线 PSRAM，当前编译配置是 quad 变体，启动会打印 `PSRAM ID read error … wrong PSRAM line mode` 与 `psram 0 B`。**不影响功能**（本项目不需要 PSRAM） |
| **WebSocket 控制连接** | 设备主动建立 WSS 长连接；断线自动重连，命令默认 60 秒过期 |
| **界面按单设备做** | `/api/v1/devices` 会列出所有设备，但看板只呈现一个 |
| **24 h 内存泄漏观测未做** | 载荷里始终带 `heap_int` / `heap_min`，可直接查库统计 |
| **阀门隔离性验收未做** | 目标：断网时阀门脉冲时长与联网时无可测差异 |
| **PID 参数未随 SHT30 复核** | 增益是围绕上一代传感器定的；换型后过冲风险可能变化 |

---

## 8. 一页速查

| 我要… | 命令 / 地址 |
|---|---|
| 看看板 | `https://175.24.182.190/`（Basic Auth） |
| 看设备日志 | `pio device monitor -p COM22` |
| 烧生产固件 | `pio run -t upload --upload-port COM22` |
| 烧开发固件（无传感器） | `pio run -e sim -t upload --upload-port COM22` |
| 确认凭据文件被忽略 | `git check-ignore -v src/Secrets.h src/ServerCaCert.h` |
| 改设备显示名 | 服务器上编辑 `/etc/incubator/devices.json`（热重载，无需重启） |
| 查最新样本 | 服务器 sqlite 查询（§4.3） |
| 看服务日志 | `journalctl -u incubator-api -f` |
| 手工重跑契约测试 | 见 `server/README.md` §5 |

---

## 9. 本文件变更记录

| 日期 | 变更 |
|---|---|
| 2026-09-19 | 首次建立。覆盖日常查看、设备上线、排障、开发用模拟构建、凭据安全、已知限制 |
