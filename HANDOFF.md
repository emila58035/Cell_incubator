# Cell Incubator 项目交接文档

> 面向接替工作的其他 AI harness / 开发者 / 未来的自己。
> 本文档由 DSH 于 **2026-09-18 18:35 (Asia/Shanghai)** 汇总生成，所有事实均经过实际核实，核实命令与原始输出附在对应位置。
> 前序工作主要在 Codex（VSCode 扩展）中完成，会话语料位于 `C:\Users\Emila_58035\.codex\sessions\`。
>
> **修订 R1 · 2026-09-18（Pi harness 会话）**：① 更正 §4.2 的改动归属、文件清单与编译状态（原文误判为来源不明且未验证）；
> ② §2.1 补 `platformio.exe` 完整路径；③ 新增 §4.1.1 记录工作树全量变更；④ 依据使用者「手机 4G 跳未备案页」的反馈
> 与本机 Host 头拦截实测，新增 §5.5 并**重写 §6**（原 DNS-01 路线在备案前无意义，改推 IP 直连）；
> ⑤ 新增 §3.3.1 载荷 Schema 缺口、§3.4 并发模型冲突。原始核实内容与出处均予保留。

---

## 0. 一分钟速览（TL;DR）

| 项 | 状态 |
|---|---|
| 固件（ESP32-S3） | **已开发完成**，可编译运行；本地温控功能齐全 |
| 固件联网改造 | **尚未开始编码**，仅有设计方案（见 §3.2） |
| 服务器端部署 | **约 40%**，卡在 HTTPS 证书签发 |
| 监控网页 / FastAPI / SQLite | **尚未创建** |
| 当前硬阻塞 | 域名 `*.lxc-away.site` **未完成 ICP 备案**，被腾讯云在网页层**全网拦截**（使用者手机 4G 实测确认，§5.5.1） |
| 🆕 关键转机 | **实测：拦截只匹配 Host 头中的域名，用 IP `175.24.182.190` 直连返回 200**（§5.5.2）→ 存在无需备案的可用路径 |
| 推荐下一步 | 双线并行：**IP 直连 + 自签 CA 立即打通**（§6.1），同时**启动 ICP 备案**（§6.2；域名在 DNSPod、服务器在腾讯云、`.site` 已获工信部批复，前置条件全满足） |

**最重要的三句话**：

1. **不要再用域名申请证书**——`*.lxc-away.site` 未备案被全网拦截，HTTP-01 / DNS-01 签出的证书在备案解封前对浏览器和 ESP32 都无意义，重试只会白撞 Let's Encrypt 频率限制。改走 **IP 直连**（§6.1）。
2. **两批改动均已按主题提交并推送**（R2 完成，详见 §4.1.1）：① `533191d` 加热开关解耦 ② `e7e143a` AHT20→SHT30 迁移。`HeaterController.cpp` 中混装的 `SetSampleTime` 改名行已拆进批次二，**两个提交各自实测 `pio run` SUCCESS**。待实机验证的只剩 **PID 参数未随 SHT30 复核**这一项。
3. **联网并发模型尚未裁决**（§3.4）——`HTTPClient` 是同步阻塞 API，与 §8 红线「不得阻塞主循环」冲突，实测会拉长阀门脉冲、漏算 PID 积分。**编码前必须定**。

---

## 1. 项目背景与目标

### 1.1 是什么

`Cell_incubator` 是一个**细胞培养箱控制器**固件项目：

- 硬件平台：Adafruit Feather ESP32-S3
- 传感器：SHT30（温湿度，I²C，地址 0x44）+ SenseAir S8（CO₂，UART）
- 执行器：加热 PWM、风扇、LED 照明、CO₂ 排气阀门
- 交互：旋转编码器 + 按键 + 320×240 TFT 显示屏
- 控制：PID 闭环控温，双组参数（保守/激进），按偏差带切换

### 1.2 当前阶段的目标

给培养箱加**联网能力**，实现远程查看。已明确的范围边界：

```text
1 台培养箱
ESP32-S3 自带 Wi-Fi
第一阶段只做「远程查看」，不做远程控制
服务器端自建，部署在已有的腾讯云服务器上
```

**硬性约束**：联网功能**不得影响本地温控**。Wi-Fi 或服务器不可用时，主循环必须继续正常执行本地 PID、阀门、风扇控制，联网只做非阻塞退避重试；初期只允许少量内存缓存，不频繁写 NVS（避免闪存磨损）。

---

## 2. 环境与访问信息

### 2.1 开发机（Windows）

```text
工作区：      D:\PlatformIO\Cell\Cell_incubator
GitHub：      https://github.com/emila58035/Cell_incubator.git  (origin)
              可见性：**private**（R2 于 2026-09-18 由 public 转为私有；
              转私有前实测 forks=0 / watchers=0 / stars=0，无外部公开副本）
分支：        master（当前，与 origin/master 同步）
              ⚠️ 本仓库已于 R4 改写历史，所有提交哈希都变了——**不要在文档里写死 HEAD 哈希**，
              要看当前值请跑 `git log -1`；历史改写记录见 §4.6，新提交链见 §4.4 表格
              backup-master 已在 R2 删除——它是 master 的祖先、无独有提交、
              且早已随 master 推到远端，属纯冗余。复原：`git branch backup-master b392206`
工具链：      PlatformIO / VSCode（pio 不在 PATH，但可用完整路径调用：
              C:\Users\Emila_58035\.platformio\penv\Scripts\platformio.exe run
              —— R1 已验证可正常构建，2026-09-18 实测 [SUCCESS]）
Python：      D:\anaconda 可用
Node.js：     v24.16.0（D:\Program Files\nodejs）
```

### 2.2 腾讯云服务器

```text
公网 IP：     175.24.182.190
系统：        Ubuntu，Nginx 1.24.0，Python 3.12
域名：        incubator.lxc-away.site  →  A 记录 175.24.182.190
DNS 托管：    DNSPod（NS: sinew.dnspod.net）
Certbot 账户：已注册（申请时无需再传 --email；该账户使用邮箱 647337479@qq.com）
```

**⚠️ 关于 SSH：当前开发机上没有配置任何 SSH 密钥或 `~/.ssh/config`**（已核实：`C:\Users\Emila_58035\.ssh` 不存在）。
因此**当前 harness 无法直接登录服务器**。前序所有服务器操作都是「AI 给出命令 → 用户在服务器终端执行 → 把输出贴回来」的模式。
如果希望接手方直接操作服务器，需先配置 SSH 别名（`~/.ssh/config` 中定义 `Host incubator-server`），然后明确授权。

### 2.3 服务器上的既有服务（不要动）

- `/root` 中约 **8.7 GB 是饥荒专用服务器文件**——与本项目无关，**严禁触碰**。
- `/etc/nginx/conf.d/ota.conf`：默认站点，`location /fw/` 提供固件 OTA 下载。**必须保持可用**。
- 宝塔面板生成的 `/www/server/panel/vhost/nginx/lxc-away.site.conf` **并未被实际加载**，修改它不会生效。实际生效的只有 `/etc/nginx/nginx.conf` + `/etc/nginx/conf.d/*.conf`。

```text
风险提示：服务器磁盘仅剩约 8.3 GB；系统日志约 2.1 GB 偏大。
        确认不需要旧日志后可执行 sudo journalctl --vacuum-size=500M 释放约 1.5 GB。
```

---

## 3. 技术方案

### 3.1 总体架构

```text
ESP32-S3 ──HTTPS POST──▶ Nginx (443, 只暴露这个)
                            ├─ /               静态监控网页
                            └─ /api/v1/*       反向代理
                                   ↓
                            FastAPI (127.0.0.1:8000，不监听公网)
                                   ↓
                            SQLite (/var/lib/incubator/data.db)
```

设计原则：**不引入 Docker / InfluxDB / Grafana**，复用服务器上已有的 Nginx 与宝塔证书目录，尽量轻量。

### 3.2 固件改造设计（尚未编码）

现有代码结构已经适合接入：所有状态集中在 `src/SystemState.h` 的 `struct SystemState` 中，
联网模块**只需读取它**，无需改动 PID、传感器、阀门、风扇逻辑。

计划新增：

```text
src/NetworkConfig.h        非敏感联网参数（服务器地址、上传周期）
src/WiFiManager.h/.cpp     Wi-Fi 连接与断线重连
src/TelemetryClient.h/.cpp HTTPS 状态上传与鉴权
（可选）src/TimeSync.h/.cpp NTP 时间同步
```

`src/main.cpp` 的 `loop()` 中增加两个**非阻塞**调用：

```cpp
wifiManager.update(now);
telemetryClient.update(now, systemState);
```

上传字段可直接取自 `SystemState`（字段名已核实）：

```text
temperature / temperatureValid
humidity / humidityValid
co2 / co2Valid
setpoint
heaterOutput / heaterDuty / heatingEnabled
lightEnabled / lightBrightness
fanEnabled
valveOpen
```

周期建议：传感器维持现状（温湿度 1 s、CO₂ 5 s），上传 10 s 一次。

敏感配置（Wi-Fi 名/密码、设备 API 密钥）走本地 `secrets.ini` + PlatformIO 编译参数注入，
并加入 `.gitignore`；仓库只保留不含真实密码的 `secrets.example.ini`。
**密钥、密码一律不得提交到 GitHub。**

### 3.3 服务器端 API 设计

首批接口（第一阶段只读）：

```text
POST /api/v1/telemetry              ESP32 上传状态
GET  /api/v1/devices/{id}/latest    最新状态
GET  /api/v1/devices/{id}/history   历史曲线数据
GET  /api/v1/devices                设备列表
GET  /healthz                       健康检查
```

请求格式：

```http
POST /api/v1/telemetry
Authorization: Bearer <设备专属密钥>
Content-Type: application/json
```

```json
{
  "device_id": "incubator-001",
  "timestamp": 1760000000,
  "temperature": 37.2,
  "humidity": 58.4,
  "co2": 5.1,
  "target_temperature": 37.0,
  "heater_pwm": 42,
  "fan": true,
  "light": false,
  "valve": false,
  "firmware_version": "0.1.0"
}
```

数据库表：`devices`、`telemetry`、`alerts`。

#### 3.3.1 ⚠️ 载荷 Schema 待定项（R1 新增，编码前须裁决）

对照 `src/SystemState.h` 实际字段，上述 JSON 示例存在 4 处缺口：

| # | 问题 | 现状 | 建议 |
|---|---|---|---|
| 1 | **CO₂ 单位不一致** | 示例 `"co2": 5.1` 是百分比(float)；但固件 `SystemState.co2` 是**整数 ppm**，TFT 按 `ppm` 显示，阀门阈值 25000/40000/48000 对应 2.5%/4.0%/4.8% | 上报**整数 ppm**，看板负责换算，无损且与本机显示一致 |
| 2 | **缺有效性标志** | 载荷无 `temperatureValid` / `humidityValid` / `co2Valid` | 必须补 `*_ok`。否则看板无法区分「传感器故障」与「读数恰为 0」，§4.2 加热解耦改动的价值在远程端完全体现不出来 |
| 3 | **缺加热开关状态** | §3.2 已把 `heatingEnabled` 列为数据源，但 §3.3 载荷只有 `heater_pwm` | 补 `heater_on`。开关 ON + 传感器故障时 `heater_pwm=0`，与「用户主动关加热」的 0 含义完全不同 |
| 4 | **`timestamp` 需设备对时** | §3.2 把 NTP 列为「可选」，但载荷含 `timestamp`；不启用 NTP 则设备无从填写 | 设备只报 `uptime_s`，服务器用接收时刻打权威时间戳；或明确启用 `TimeSync` 模块 |

可选补充：`rssi`、`free_heap`（远程排查内存泄漏）、`wifi_connected`。

### 3.4 ⚠️ 并发模型待裁决（R1 新增，编码前必须决定）

§3.2 要求 `loop()` 中非阻塞调用：

```cpp
wifiManager.update(now);
telemetryClient.update(now, systemState);
```

但 arduino-esp32 的 `HTTPClient` / `WiFiClient` 是**同步阻塞 API**，`POST()` 本身无法非阻塞
（Wi-Fi 连接可以靠轮询 `WiFi.status()` 做到非阻塞，HTTP 请求不行）。最坏情况
（connectTimeout + responseTimeout ≈ 3 s）会卡住主循环，与 §8 红线直接冲突：

| 受影响模块 | 机制 | 后果 |
|---|---|---|
| `ValveController::pulseValve()` | 靠 `millis()-lastOpenTime >= pulseTime` 关阀 | 主循环卡 3 s → **本该 500 ms 的短脉冲变成 3 s，CO₂ 过量注入** |
| `HeaterController` PID | `PID_v1::Compute()` 按 `SampleTime(1s)` 节流，积分增量绑定 SampleTime 而非真实时差 | 卡 3 s 漏算 2 次积分，控温响应变慢 |
| `SHT30Sensor` 采样 | 1 s 周期被推迟 | 控温基于过期读数 |

**推荐调和方案**（既满足 §8 红线，又让 §3.2 接口字面成立）：

> 保留 §3.2 的模块划分、文件命名、`loop()` 调用形式**完全不变**；只把 `TelemetryClient` 内部的阻塞部分
> 放进钉在 **Core 0** 的 FreeRTOS 任务（ESP32-S3 双核，Arduino `loopTask` 在 Core 1，ESP-IDF Wi-Fi 协议栈
> 也在 Core 0，天然同侧）。`telemetryClient.update(now, state)` 从 loop 调用时只做
> 「`portMUX` 临界区取快照 + 回读状态」，微秒级返回。

备选：按 §3.2 原样在主循环同步 POST，但把超时压到 1 s 内、上报间隔拉到 30 s，接受阀门脉冲误差。

---

## 4. 当前进度

### 4.1 固件侧

**主功能已完成**，`master` 上最后一次提交：

```text
a7eaf17  2026-09-14 17:52:16 +0800  Initial commit: Cell incubator firmware
```

源码布局（已核实）：

```text
src/
├─ main.cpp                主循环（setup/loop，非阻塞模式）
├─ Config.h                全部引脚、时序、PID 参数常量
├─ SystemState.h/.cpp      全局状态结构体（联网模块的数据来源）
├─ SHT30Sensor.h/.cpp      温湿度传感器
├─ SenseAirS8Sensor.h/.cpp CO₂ 传感器（UART1，非阻塞轮询）
├─ HeaterController.h/.cpp 加热 PID 控制
├─ FanController.h/.cpp    风扇控制
├─ ValveController.h/.cpp  CO₂ 阀门控制
├─ LEDController.h/.cpp    照明控制
├─ InputManager.h/.cpp     编码器 + 按键输入
├─ DisplayManager.h/.cpp   TFT 显示
└─ SettingsStorage.h/.cpp  设置持久化（手动保存）
platformio.ini             env: adafruit_feather_esp32s3
```

**联网模块尚未创建**——`src/` 下没有 `WiFiManager`、`TelemetryClient`、`NetworkConfig` 等文件。

#### 4.1.1 ⚠️ 工作树全量变更（R1 实测 `git status`，修正 §4.2 的「3 个文件」说法）

上方布局反映的是**工作树**状态；相对 `master`(a7eaf17) 的完整差异如下，分属**两批独立工作**：

> **🆕 R2 更新（2026-09-18）：下面这份 `git status` 清单已是历史快照，两批改动都已提交。**
> **⚠️ R4（2026-09-18）：本仓库已用 `git filter-repo` 改写历史，所有提交哈希都变了**（旧哈希仅存于备份）。
> 本文档中出现的全部哈希已按 `.git/filter-repo/commit-map` 重映射为**改写后的新值**，可直接 `git log` 对证；
> 若你手上的 clone 是 R4 之前做的，必须重新 clone（详见 §4.6）。
> 提交链（R2/R3，均已推送）：停止跟踪 tmp/（此提交在 R4 中因变空而被丢弃）→ `533191d` 批次一
> → `e7e143a` 批次二 → `53948ea` `.gitattributes` → `f8d6a4f` `docs/REMOTE_MONITORING.md` → `7762b82` `HANDOFF.md`。
> **R3：两份文档已入库**（前提是仓库已转 private），工作树现在**完全干净**，无任何未跟踪文件。

```text
【批次一：加热开关解耦】 Pi harness 会话完成，已编译通过，待实机验证（详见 §4.2）
 M src/HeaterController.cpp      ← 注意：本文件同时含批次二的一行改名，提交须拆分
 M src/HeaterController.h
 M src/InputManager.cpp

【批次二：AHT20 → SHT30 迁移】 使用者本人进行中（R2 已提交为 `e7e143a`，含下面的改名行）
 D src/AHT20Sensor.cpp
 D src/AHT20Sensor.h
?? src/SHT30Sensor.cpp           ← 未跟踪，尚未 git add
?? src/SHT30Sensor.h
 M platformio.ini                lib_deps: dvarrel/AHT20@^1.0.2 → robtillaart/SHT85@^0.6.3
 M src/Config.h                  Aht* 常量 → Sht*（ShtAddress=0x44，SDA=1/SCL=2 不变）
 M src/main.cpp                  #include 与 climateSensor 类型改为 SHT30Sensor
 M src/HeaterController.cpp      SetSampleTime(AhtSampleIntervalMs → ShtSampleIntervalMs)

【文档，均未跟踪】
?? HANDOFF.md
?? docs/                         docs/REMOTE_MONITORING.md（远程监控详细设计，R1 已对齐本文档）
```

**接手注意**：
- ~~批次二**未完成且未经构建验证**~~ → **R2 实测：`pio run` SUCCESS**（RAM 12.1% / 39548 B，Flash 33.1% / 477781 B）。
  另将中间提交 `533191d` 单独检出到临时 worktree 构建，同样 SUCCESS，证明按主题拆分未破坏可编译性。
  「未完成」仍然成立：缺的是**实机验证**与下面的 **PID 复核**，不是编译。
- ~~`HeaterController.cpp` 混装两批改动 → 提交时用 `git add -p` 拆开~~ → **R2 已拆分完成**：
  `SetSampleTime` 的常量改名行进 `e7e143a`，加热策略改动进 `533191d`（该提交 diff 已确认不含 `SampleIntervalMs`）。
- SHT30 精度（典型 ±0.2 ℃）优于 AHT20（±0.3 ℃），对控温是有利变更；但 `Config.h` 的 PID 参数
  （保守 Kp=10/Ki=0.1、激进 Kp=20、切换带 3 ℃）未随之复核，迁移完成后建议重新整定。
- `robtillaart/SHT85` 库同时支持 SHT30/SHT31/SHT85，类名与 `Config.h` 中 `ShtAddress=0x44` 需与实物 ADDR 引脚电平一致。

### 4.2 ⚠️ 加热开关解耦改动（R2：已提交为 `533191d`）

**🆕 R2 状态**：本节的改动**已提交并推送**，工作树已不再承载它。以下内容保留作为改动语义的说明。
本节只讲**批次一：加热开关解耦**（`HeaterController.cpp/.h`、`InputManager.cpp`）；
全量差异（含批次二 SHT30 迁移与文档）见 §4.1.1，版本库整备汇总见 §4.4。

> **✅ R1 更正（归属已确认）**：这 3 个文件的改动**由 Pi harness 会话在使用者明确要求下完成**，
> 并非来源不明的本地/IDE 修改。需求原文：① 开关关 → 不加热；② 开关开 + 传感器正常 → 加热，
> 传感器故障 → 停止加热**但开关保持 ON**；③ 开关仅由用户输入控制。改动语义与下表描述完全一致。
>
> **并且已编译验证通过**（原文档称「未经过编译验证」，实为当时未找到 pio 的路径问题，见 §2.1）：
>
> ```text
> platformio.exe run → [SUCCESS] Took 6.11 seconds
> RAM:   12.1%  (39524 / 327680 bytes)
> Flash: 33.1%  (477045 / 1441792 bytes)
> ```
>
> **仍未做**：实机烧录验证。
>
> 文件修改时间：`HeaterController.h` / `InputManager.cpp` = 2026-09-17 15:56:53；
> `HeaterController.cpp` = 2026-09-18 18:40:50（因使用者的 SHT30 迁移又改了一行，见 §4.1.1）。

改动语义（加热开关与传感器故障解耦）：

| 文件 | 改动前 | 改动后 |
|---|---|---|
| `HeaterController::update` | 温度无效时 `stop(state, true)`，**会自动把 `heatingEnabled` 关掉** | 温度无效时仅暂停加热（输出 0），**开关保持 ON**；新增 `sensorFault_` 标志并打印 `[HEATER] Sensor fault: heating paused, switch stays ON`，恢复时打印 `[HEATER] Sensor recovered: resuming control` |
| `HeaterController::stop` | 签名 `stop(SystemState&, bool disableRequest)`，含自动关闭开关的副作用 | 签名简化为 `stop(SystemState&)`，**不再有副作用** |
| `HeaterController.h` | — | 新增成员 `bool sensorFault_ = false;` |
| `InputManager`（SetTemperature 分支） | 温度无效且加热关闭时**拒绝**开启加热，打印 `[INPUT] Heating rejected: temperature is invalid` | **无条件**切换 `heatingEnabled` |

设计意图（**R1：已由使用者确认，非推测**）：**开关状态完全由用户输入决定，系统不再替用户自动关闭加热**；
传感器故障只暂停加热输出，恢复后自动继续。这与「联网不得影响本地控制」的取向一致。
**编译已通过**（见上方 R1 更正），**实机验证未做**。

```text
接手建议（R2 更新）：
1. 归属与编译状态已确认（见上），无需再问；仍待办：实机烧录验证。
2. ~~提交时机：等 SHT30 迁移编译通过后用 git add -p 拆两个 commit~~ → **R2 已完成**：
   已拆为 `533191d`（加热解耦，实际消息 "Keep heating switch under user control during sensor faults"）
   与 `e7e143a`（SHT30 迁移，含 SetSampleTime 改名行）。
3. ~~不要擅自 git checkout -- 丢弃在途改动~~ → R2 后已无未提交的代码改动。
   ~~未跟踪文档只有一份副本、切勿 `git clean -fdx`~~ → **R3：`HANDOFF.md` 与 `docs/` 已提交并推送（`7762b82`/`f8d6a4f`），
   现在有远端副本，`git clean -fdx` 不再会丢失它们**。
4. 新增待办：SHT30 的 PID 参数复核（见 §4.1.1、§4.4 末尾）。
```

### 4.3 服务器侧（约 40%）

**已完成**：

1. ✅ 只读体检：CPU/内存充足，Python 3.12 可用，原仅监听 80 端口。
2. ✅ 摸清 Nginx 真实配置来源：主配置 `/etc/nginx/nginx.conf`，实际加载 `/etc/nginx/conf.d/ota.conf`；宝塔的 `lxc-away.site.conf` 未被加载。
3. ✅ `sudo apt install -y python3-venv python3-pip certbot`。
4. ✅ 创建服务用户与目录：
   - `incubator` 系统用户（home `/opt/incubator`，shell `nologin`）
   - `/opt/incubator/app`、`/var/lib/incubator`（750，属 incubator）
   - `/var/www/incubator` 及 `.well-known/acme-challenge`（755，root:www-data）
   - `/etc/incubator`（750，root:incubator）
5. ✅ 创建并生效 `/etc/nginx/conf.d/incubator.conf`（80 端口临时站点）：
   `nginx -t` 通过 → `systemctl reload nginx` → `curl http://incubator.lxc-away.site/`
   返回 200 且内容为 `incubator service setup`，**原 `/fw/` OTA 服务未受影响**。
6. ✅ Certbot 账户已注册（邮箱 `647337479@qq.com`）。

**未完成**：

- ❌ 证书签发（被阻塞，见 §5）
- ❌ FastAPI 应用与虚拟环境
- ❌ SQLite 数据库与表结构
- ❌ `systemd` 服务
- ❌ 443 端口 HTTPS 反向代理
- ❌ 只读监控网页
- ❌ 固件联网代码与联调

### 4.4 🆕 R2 版本库整备（2026-09-18，接手方必读）

本节把 R2 一次性做完的版本库操作汇总成可验证的清单。

| 提交 | 内容 | 验证 |
|---|---|---|
| ~~`6046de4`~~ | 停止跟踪 `tmp/`（4 张 `sch1*.png` 约 1.6 MB） | 当时校验：`git ls-files tmp` 为空。**该提交已在 R4 历史改写中被丢弃**——它只删 tmp/ 内容，移除 tmp 后成了空提交（见 §4.6） |
| `533191d` | 批次一 加热开关解耦 | **单独检出到临时 worktree 后 `pio run` SUCCESS** |
| `e7e143a` | 批次二 AHT20→SHT30 迁移 | `pio run` SUCCESS（RAM 12.1%，Flash 33.1%） |
| `53948ea` | 新增 `.gitattributes`（`* text=auto eol=lf` + 二进制声明） | 29 个文本文件全部 `i/lf w/lf`；`git add --renormalize .` 产生**空 diff**，证明对象库本来就是纯 LF，无需改写历史 |
| `f8d6a4f` | 新增 `docs/REMOTE_MONITORING.md`（740 行） | 入库前复确认仓库为 private；全文扫描无凭据（`htpasswd` 只是 nginx 里的**路径**，非密码值） |
| `7762b82` | 新增 `HANDOFF.md`（815 行，含本节） | 同上；两文件均以 `i/lf w/lf` 入库，`.gitignore` 不拦截 |
| `f091b6e` | **新增 `AGENTS.md`**（187 行，仓库根）——所有 AI 会话的结构维护规则 | 故意不含具体 IP/域名，以免自己成为泄露源；待提交集跑完自带扫描后才发现它命中的是自己写的那条正则 |
| `38d96d8` | 修正 `AGENTS.md` 的提交前扫描命令 | 原命令对整个暂存区跑，会**自匹配** `AGENTS.md` 里的模式字面量；改为限定路径（`src` `platformio.ini` `.gitignore` `.gitattributes`）+ 文档类文件走路径白名单 |

其他：

- **本地分支 `backup-master` 已删除**（`git branch -d`，git 自行校验已合入 master）。复原：`git branch backup-master b392206`。
- **仓库由 public 转为 private**（转私有前实测 `forks_count=0 / subscribers=0 / stargazers=0`，无外部公开副本）。匿名访问 `api.github.com` 现返回 404，已复核。
- ~~**`tmp/pdfs/sch1*.png` 仍在历史中**……需 `git filter-repo` + 强推~~ → **R4 已完成**：原理图与所有 `tmp/` 内容已从**全部历史提交**中移除，`.git` 从 1.74 MB 降到 **0.11 MB**。命令、校验方法、备份位置与残留风险见 §4.6。
- **代理问题（重要环境差异）**：本机 `github.com:443` 直连**超时**，但 `api.github.com:443` 可直连；`127.0.0.1:7890` 有代理进程在监听，但系统 `ProxyEnable=0`、git 也没配 `http.proxy`，所以 `git push` 会直接失败。本轮用一次性参数绕过（**未写入任何配置**）：

  ```powershell
  git -c http.proxy=http://127.0.0.1:7890 push origin master
  ```

  使用者已明确选择**不持久化**。因此**每次网络操作都得带这个 `-c` 参数**，或先打开代理开关。
  另：Windows PowerShell 5.1 调 GitHub API 需先 `[Net.ServicePointManager]::SecurityProtocol = 'Tls12'`，否则报「基础连接已经关闭」。

- **遗留的真实代码问题**：`Config.h` 的 PID 参数（保守 Kp=10/Ki=0.1、激进 Kp=20、切换带 3 ℃）是围绕 AHT20 定的，**未随 SHT30 复核**。
  两传感器的精度与响应曲线不同（本文§4.1.1 记为 SHT30 典型 ±0.2 ℃ 优于 AHT20 ±0.3 ℃），且 I2C 采样节奏、
  故障时的 `temperatureValid` 跳变行为也已改变，需重新整定（具体方法见 §4.5）。
  服务器 IP、域名、Nginx 布局等情报自 R3 起**已进入版本历史**（`f8d6a4f`/`7762b82`）。
  它们安全的唯一前提是仓库保持 **private**：
  **任何情况下不要把这个仓库改回 public、不要邀请外部协作者、不要把 clone 镜像到其他公开位置**；
  否则需先改写历史抹除这两个文件才能恢复公开。

### 4.5 🆕 SHT30 PID 参数复核怎么做

背暂：本项目加热用的是**带滞环的双增益 PID**（`Config.h`：保守 Kp=10/Ki=0.1、激进 Kp=20，切换带 3 ℃），
`PID` 库以 `Config::ShtSampleIntervalMs`（1 s）为采样周期。换传感器后需要重新确认三件事：

1. **自洽性**：采样周期未变（仍 1 s），`SetSampleTime` 已随之改名，这一项无风险。
2. **滞后变化**：SHT30 与 AHT20 的封装、响应时间（τ）、自热效应不同，会让回路纯滞后变化，
   直接决定 Kp 上限。现象上表现为：加热过冲变大、或输出振荡于设定值附近。
3. **失效行为变化**：R2 已把「传感器故障」从“自动关掉开关”改成“暂停输出、保持 ON”（§4.2），
   所以 I2C 偶发读失败现在**不会**再默默关掉加热，而是会反复进入暂停/恢复。需确认 `SHT30Sensor` 的
   CRC/重试失败频率低于可接受阈值，否则会出现长时间无输出。

建议做法（需实机，约 1–2 小时）：

- 先**开环**：固定 30% / 60% 占空比各跑 10 min，记录温度曲线斜率与到达时间，得到实际增益 K 与滞后 L。
  再按 SIMC/lambda 估算 Kp ≈ 1/(K·L)·(τc+L)，Ki ≈ Kp/(4·L)（τc 取 60 s）。
- 或**最简方案**：维持 Ki=0.1，把 Kp 从保守值逐步上调至出现轻微过冲后回退 20%；同时观察 3 ℃ 切换带是否仍合适。
- 比较目标：温度在设定值±0.2 ℃ 内，加热输出无 0↔满占空比振荡，且 1 小时漂移≤ 0.5 ℃。

注：细胞培养箱对过冲比恒温精度更敏感（过冲会烫死细胞），整定时宁可保守、不要追求快速到达。

### 4.6 🆕 R4 历史改写：从版本库彻底移除 `tmp/` 原理图（2026-09-18）

**背景**：`tmp/pdfs/sch1*.png`（4 张原理图，共 1.6 MB）自根提交 `696d2fe` 就进了仓库，并在仓库为 **public** 期间公开了 4 天。
R2 的取消跟踪（`git rm --cached`）只影响后续快照，**历史 blob 仍在**，所以单独做一次改写。

**执行前备份（已验证可用，务必保留）**：

```text
D:\PlatformIO\Cell\_backups\Cell_incubator-20260918-2110-mirror.git   （mirror clone）
D:\PlatformIO\Cell\_backups\Cell_incubator-20260918-2110-all.bundle   （git bundle --all，1644.7 KB）
校验：git bundle verify  →  "records a complete history" / "is okay"
恢复：git clone <mirror 目录>  或  git clone <.bundle 文件>
```

**命令**：

```powershell
pip install git-filter-repo            # 本机原本没有，需通过代理安装
git filter-repo --path tmp/ --invert-paths --force
```

**四项改写后校验（缺一不可）**：

| 校验 | 方法 | 结果 |
|---|---|---|
| 内容零丢失 | 新旧 HEAD 的 `ls-tree -r` 排除 tmp/ 后比对 | **0 差异**（逐文件 blob SHA 全一致） |
| 提交数 | `rev-list --count` | 25 → **24**；少的正是变空的 tmp/ 取消跟踪提交，消息差异 = 1 |
| 目标达成 | `git rev-list --objects --all \| Select-String '\.png$'` | 空；`.git` 1.74 MB → **0.11 MB** |
| 工作树 | `git status` | clean（filter-repo 不动工作区文件；磁盘上的 `tmp/` 仍然完好） |

**几个容易掉坑的行为，记录在此**：

1. `filter-repo` 会**主动删除 `origin` remote**（日志有 NOTICE），需 `git remote add origin <url>` + `git fetch` 重建。
2. 它**不是 git 内置命令**，依赖外部 `git-filter-repo` 可执行文件，需确保在 PATH 上。
3. 改写后**必须强推**，用 `git push --force-with-lease origin master`（而不是 `--force`，后者会直接覆盖别人的推送）。
4. **所有其他 clone 作废**，必须重新 clone。本项目只有本机一处工作副本，影响可控。
5. **改写历史 ≠ 撤回公开**。仓库曾为 public，旧提交在改写前可被任何看到 SHA 的人拉取；而且 GitHub 服务端在一时间内**仍可通过旧 commit SHA 直接访问**，Support 也可恢复。真正清除只能联系 GitHub Support。
   —— 原理图属低敏感内容，本次改写的真实收益是**仓库体积与卫生**，不应当成“消除了泄露”。
6. 旧→新哈希映射在 `.git/filter-repo/commit-map`（删除 `.git/filter-repo/` 前请先确认本文档哈希已全部重映射完成）。
7. **顺序陷阱：改写后先 `git remote add` + `git fetch` 会**把旧历史（连 png）**重新拉回本地对象库**，
   `.git` 当场涨回 1.69 MB。强推完成后必须补一次清理才能看到收益：

   ```powershell
   git reflog expire --expire=now --all
   git gc --prune=now          # 本地 .git：1.69 MB → 0.12 MB
   git rev-list --objects --all | Select-String '\.png$'   # 应为空
   git fsck --no-progress                              # 应无 dangling
   ```

8. **服务端不会立刻变干净**：强推后 GitHub 仍报 `size=1602KB`（与改写前相同），因为旧 commit 尚未被它回收，
   一时期内**仍可直接按旧 SHA 拉取**。这与上面第 5 条是同一件事：本地已彻底清除，远端要等 GitHub 自己的 GC，
   或联系 Support。

---

## 5. 中断点与阻塞分析（核心内容）

### 5.1 前序会话是怎么中断的

时间线（依据 Codex 本地存档与会话内容还原）：

| 时间 | 事件 |
|---|---|
| 2026-09-14 17:52 | 固件首次提交 `a7eaf17` |
| 2026-09-15 16:57 | Codex 主会话开始，逐步指导服务器部署（12 轮交互） |
| 2026-09-15 傍晚 | 完成目录创建与 HTTP 站点；开始申请 HTTPS 证书 |
| 2026-09-15 | HTTP-01 失败：LE 解析到 `43.174.224.202`（DNSPod 拦截页）。排查 DNS，建议等待 TTL 后用 `--dry-run` 复测 |
| 2026-09-16 | `--dry-run` 仍失败，错误相同。Codex 建议改用 DNS-01 手动验证 |
| 2026-09-16 07:58 | 用户提问「解析 IP 不对可能是 VPN，关闭后应从哪一步开始」——**此处因网络中断，会话在此终止** |
| 2026-09-16 15:58 | 新建续接会话，仅完成 1 问 1 答后再次中断 |
| 2026-09-17 15:56 | 工作树出现 3 个未提交的加热控制改动（见 §4.2，来源未确认） |
| 2026-09-16 之后 | 服务器端**无任何新进展**；证书仍未签发 |
| 2026-09-18 20:55 | **R2 版本库整备**：tmp/ 取消跟踪、两批改动按主题提交、`.gitattributes` 统一 LF、删除 `backup-master`、**仓库转 private**（全部见 §4.4）。服务器端**仍无进展** |
| 2026-09-18 21:05 | **R3**：两份文档提交并推送（`f8d6a4f` / `7762b82`），工作树首次完全干净；因 R3 而立刻过时的 5 处陈述同步修正 |
| 2026-09-18 21:15 | **R4**：`git filter-repo` 从全部历史移除 `tmp/` 原理图；25→24 提交，`.git` 1.74→0.11 MB；全文哈希重映射并强推（见 §4.6）。服务器端**仍无进展** |
| 2026-09-18 21:35 | **R5**：新增根目录 `AGENTS.md`（结构维护规则 + 禁令清单），约束后续 AI 会话不乱改结构（见 §4.4 末尾两行） |

用户当时的假设是「DNS 解析错误由 VPN 导致，关掉 VPN 重试即可」。
**下面证明这个假设不成立**——这是接手方最需要知道的一点。

### 5.2 阻塞根因：域名被网页层拦截（不是 DNS，也不是 VPN）

DSH 于 **2026-09-18 18:33** 从开发机重新实测，结果如下：

| 检查项 | 结果 | 结论 |
|---|---|---|
| `Resolve-DnsName incubator.lxc-away.site` | `175.24.182.190` | DNS **正确** |
| 阿里 DNS-over-HTTPS (`dns.alidns.com`) | `175.24.182.190` | DNS **正确** |
| Cloudflare `1.1.1.1` / Google `8.8.8.8` | `175.24.182.190` | DNS **正确** |
| 本机代理设置 | `ProxyEnable = 0`，无 `*_PROXY` 环境变量 | **VPN 未启用，不是原因** |
| TCP `175.24.182.190:80` / `:22` | 均可达 | 网络层通畅 |
| `GET http://175.24.182.190/`，Host=`incubator.lxc-away.site` | **302 → `https://dnspod.qcloud.com/static/webblock.html?d=incubator.lxc-away.site`** | **被拦截** |
| 同上，Host=`www.lxc-away.site` | 302 → webblock | 拦截范围含整个域 |
| 同上，Host=`random-test-987.lxc-away.site` | 302 → webblock | **任意子域都被拦** |
| 同上，Host=`lxc-away.com`（不存在的域名） | **200**（到达本机 Nginx） | 非 IP 段封锁 |
| 同上，Host=`test.qq.com` | **200** | 同上 |

关键推理：

- **DNS 解析已经全部正确**，所以「解析到错误 IP」的现象已消失。
- **本机没开代理**，所以 VPN 不是当前原因。
- 拦截发生在**网页（HTTP）层**，且按 `*.lxc-away.site` **整个域**匹配——因为不存在的域名反而能正常 200，
  说明这不是 IP 封锁，而是腾讯云/DNSPod 针对该域名的拦截（典型原因是域名未完成工信部 ICP 备案，
  或该域被安全策略标记）。

实战含义：

```text
Let's Encrypt 的 HTTP-01 校验请求从海外发起，命中拦截页，
永远读不到 /var/www/incubator/.well-known/acme-challenge/ 下的验证文件。
→ 继续重试 HTTP-01 必然失败，且会消耗 Let's Encrypt 的失败频率额度（约 5 次/小时）。
→ 关闭 VPN 重试同样无效。
```

参考：社区同类问题见 [通过腾讯云节点代理访问重定向到未备案提示](https://cloud.tencent.com.cn/developer/ask/256583)、
[申请 SSL 提示 Invalid response from https://dnspod...](https://www.bt.cn/bbs/archiver/tid-128547.html)。

### 5.3 复现命令（接手方建议先跑这一组）

```bash
# 在服务器上执行（终端 A）
sudo curl -i --max-time 10 -H 'Host: incubator.lxc-away.site' http://127.0.0.1/
curl -i --max-time 10 -H 'Host: incubator.lxc-away.site' http://175.24.182.190/

# 在开发机上验证拦截仍在
# PowerShell:
#   $h=New-Object System.Net.Http.HttpClientHandler; $h.AllowAutoRedirect=$false
#   $c=New-Object System.Net.Http.HttpClient($h)
#   $req=New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get,'http://175.24.182.190/')
#   $req.Headers.Host='incubator.lxc-away.site'
#   $c.SendAsync($req).GetAwaiter().GetResult().Headers.Location
```

判定：

- 服务器本机 curl 返回 `incubator service setup` → 站内链路正常，属公网侧拦截。
- 开发机仍返回 `dnspod...webblock` → 拦截**至今仍然存在**（2026-09-18 实测如此）。

### 5.4 不可用的既有证书

```text
/www/server/panel/vhost/cert/lxc-away.site/fullchain.pem
  subject: CN = lxc-away.site
  SAN:     DNS:lxc-away.site, DNS:www.lxc-away.site   ← 不含 incubator 子域
  有效期:  2026-04-30 ~ 2026-07-29                    ← 已过期
```

**结论：该证书不能用于 `incubator.lxc-away.site`**，浏览器与 ESP32 都会报域名不匹配。必须重新签发。

### 5.5 🆕 R1 实测：备案状态确认 + 拦截维度判定（2026-09-18）

#### 5.5.1 备案状态已确认：**未备案，属全网拦截**

使用者按原 §6.2 建议用**手机 4G/5G（不连 Wi-Fi、不开 VPN）**访问 `http://incubator.lxc-away.site/`，
结果**跳转到「未备案」页面**。据此判定：

> 拦截**不限于海外线路，而是全网生效**。原 §6.1「DNS-01 签发域名证书」路线在备案完成前**没有意义**——
> 证书即使签出来，浏览器与 ESP32 访问该域名仍会命中拦截页。

#### 5.5.2 拦截维度实测：**只匹配 Host 头中的域名，IP 直连不受影响**

从开发机对 `175.24.182.190:80` 发送 GET，仅改变 `Host` 头（禁用自动重定向，2026-09-18 实测原始输出）：

```text
[Host=incubator.lxc-away.site     ] -> 302  https://dnspod.qcloud.com/static/webblock.html?d=incubator.lxc-away.site
[Host=lxc-away.site               ] -> 302  https://dnspod.qcloud.com/static/webblock.html?d=lxc-away.site
[Host=175.24.182.190              ] -> 200
[Host=nonexistent-domain-check.com] -> 200
[Host=(default)                   ] -> 200
```

**结论**：
- 拦截发生在**腾讯云网络边界**，判定依据是 HTTP `Host` 头（443 上对应 TLS `SNI`）中的**域名**，
  与 DNS 由谁托管**无关**，与请求来源是否在境内**无关**（Let's Encrypt 海外校验节点同样被拦）。
- **用 IP 地址访问完全不被拦截**（200，直达本机 Nginx）。

→ 由此得到一条**无需备案即可立即打通**的路径（§6.1）。

复现命令（PowerShell，开发机；注意需 `System.Net.HttpWebRequest`，
§5.3 里的 `System.Net.Http.HttpClient` 在未 `Add-Type` 的 Windows PowerShell 5.1 下会报「找不到类型」）：

```powershell
$req=[System.Net.HttpWebRequest]::Create('http://175.24.182.190/')
$req.AllowAutoRedirect=$false; $req.Timeout=12000
$req.Host='incubator.lxc-away.site'   # 换成 '175.24.182.190' 做对比
($req.GetResponse()).StatusCode
```

#### 5.5.3 域名与备案可行性核查（R1）

| 项 | 结果 | 来源 |
|---|---|---|
| 域名状态 | `active`，注册 2026-03-24，到期 2027-03-24 | RDAP `rdap.org/domain/lxc-away.site` |
| 注册商 | IANA ID **1697 = DNSPod, Inc.**（中国，腾讯系） | RDAP + IANA 注册商库 |
| NS | `sinew.dnspod.net` / `rose.dnspod.net` | RDAP（与 §2.2 一致） |
| `.site` 后缀可否备案 | ✅ **可以**。工信部信管函〔2016〕564号（2016-12-29）已批复 .SITE 为境内认可的域名注册管理机构 | 见 §6.2 参考链接 |

**含义**：域名在境内注册商（DNSPod）、服务器在腾讯云、`.site` 后缀已获工信部批复
→ **备案前置条件全部满足，无需转移域名**，直接走腾讯云备案控制台即可。
唯一待使用者确认：域名**实名认证信息**是否与拟备案主体（个人/企业）一致。

---

## 6. 推荐后续方案

### 6.1 🆕 方案 A（R1 推荐，立即可用）：IP 直连 + 自签 CA，绕开域名拦截

> **⏸ 原「方案 A：DNS-01 签发域名证书」已降级为暂缓路线**（移至 §6.1.9）——§5.5.1 确认属全网未备案拦截，
> 域名证书签出来也无法被浏览器/ESP32 使用。该流程仅在 §6.2 备案完成后才有意义。

依据 §5.5.2 实测：**拦截只认 Host 头里的域名，用 IP 访问返回 200**。因此可完全绕开域名：

```text
ESP32 ──HTTPS──▶ https://175.24.182.190/api/v1/telemetry
                  Host / SNI = 175.24.182.190   ← 不含被拦域名，不触发拦截
看板   ────────▶ https://175.24.182.190/         ← 同上
```

**证书选型（三选一）**

| 选项 | 说明 | 评价 |
|---|---|---|
| **A1 自签 CA（R1 推荐）** | 自建根 CA（10 年）+ 服务器叶子证书；ESP32 用 `WiFiClientSecure::setCACert()` 内嵌该 CA | **零外部依赖、零续期风险、当天可通**。代价：浏览器首次需导入 CA 或接受警告（内部使用可接受） |
| A2 Let's Encrypt **IP 证书** | LE 自 **2026-01-15** 起 GA 支持 IP 地址证书；强制 `shortlived` profile → **仅 6 天（160 小时）有效期**；certbot **≥5.4** 才支持 `--webroot --ip-address`，且**不能自动装入 nginx**，须手写 `ssl_certificate` 并配 `--deploy-hook` reload | 公开可信、浏览器无警告。代价：Ubuntu apt 的 certbot 版本远低于 5.4（需 pip/snap 升级）；**6 天续期链条一旦断裂设备立即失联**，对无人值守培养箱偏脆弱 |
| A3 明文 HTTP | 看板走 `http://175.24.182.190/`（实测 200） | 最快，但 Basic Auth 凭据明文传输，**不建议** |

**推荐 A1**：培养箱是无人值守设备，**续期周期越短风险越高**；自签 CA 内嵌固件后，服务器叶子证书可随时用同一 CA 重签而**无需重新烧录固件**。

**A1 落地要点**

```bash
# 1. 生成 10 年期根 CA（ca.key 离线保管，权限 600，切勿进仓库）
openssl genrsa -out incubator-ca.key 4096
openssl req -x509 -new -key incubator-ca.key -sha256 -days 3650 \
  -subj "/CN=Incubator Internal CA" -out incubator-ca.pem

# 2. 用该 CA 为服务器 IP 签叶子证书（SAN 必须含 IP，否则 ESP32 与浏览器都报不匹配）
openssl genrsa -out server.key 2048
openssl req -new -key server.key -subj "/CN=175.24.182.190" -out server.csr
openssl x509 -req -in server.csr -CA incubator-ca.pem -CAkey incubator-ca.key \
  -CAcreateserial -days 825 -sha256 -out server.pem \
  -extfile <(printf "subjectAltName=IP:175.24.182.190")

# 3. Nginx 443 指向 server.pem / server.key，按 §6.3 顺序施工
# 4. 固件内嵌 incubator-ca.pem 内容（经 secrets.ini 注入；CA 私钥绝不可进仓库或固件）
```

**⚠️ 因改用 IP 访问而新增的安全要求**（IP 无域名遮蔽、全网扫描即可发现，且本机 80 端口已公开提供 `/fw/` OTA）：

- 设备上报：`Authorization: Bearer <key>`（§3.3 已定），服务端**常量时间比较**
- 看板：**必须加 Basic Auth**——原文档未规定，R1 补为**强制项**
- POST 端点限流（建议 20 req/min/IP）
- 服务端输入范围校验（`temperature` -40~85、`humidity` 0~100、`heater_pwm` 0~255）
- Nginx 只暴露 443（+ 既有 80 的 `/fw/`），FastAPI 只监听 `127.0.0.1:8000`

**⚠️ 服务器地址必须可配置**：`ServerUrl` 走 `secrets.ini` → build_flags 注入（§3.2）。
备案完成后从 IP 切回域名时，只需改 `secrets.ini` 重新编译；若进一步存入 NVS 则连重新烧录都省了。

#### 6.1.9 备案完成后的域名证书签发（原方案 A，⏸ 暂缓执行）

> **R1：以下 DNS-01 流程在 §6.2 备案通过、拦截解除前不要执行**——签出来也无法使用，
> 且会消耗 Let's Encrypt 频率额度。保留此节供备案完成后使用。

不依赖公网 HTTP 访问，**不需要 Nginx 运行、不需要 webroot 目录**，与 80/443 配置无关。

```bash
sudo certbot certonly --manual --preferred-challenges dns -d incubator.lxc-away.site
```

Certbot 会暂停在 `Press Enter to Continue`，并给出 TXT 值。到 DNSPod 控制台添加：

```text
类型：      TXT
主机记录：  _acme-challenge.incubator        ← 只填这一段，DNSPod 自动补 .lxc-away.site
记录值：    <certbot 输出的随机字符串，原样复制，不加引号>
线路：      默认
TTL：       600
```

验证生效后（**先验证，再按回车**）：

```bash
dig +short TXT _acme-challenge.incubator.lxc-away.site
```

> 现已核实：`_acme-challenge.incubator.lxc-away.site` 与 `lxc-away.site` 目前**均无 TXT 记录**，
> 因此是全新添加，不存在覆盖旧值的问题。

重要说明：

- 该 TXT 记录与现有 `incubator` **A 记录是两个不同的 DNS 名字**，添加 TXT 不影响、不覆盖 A 记录，
  服务入口 `175.24.182.190` 保持不变。这正是 DNS-01 能安全绕开拦截的原因。
- 若 certbot 一次给出**两串** TXT 值，两串都要添加，不能覆盖。
- **不要使用 `certbot --nginx`**——它会自动改写站点配置，而本机宝塔目录与实际生效的 `/etc/nginx/conf.d/` 是两套，极易搞乱现有 OTA 服务。
- **不要用 `--dry-run` 复测**，直接正式申请即可（账户已注册）。
- 成功标志：`/etc/letsencrypt/live/incubator.lxc-away.site/fullchain.pem` 出现。
  核对命令：

```bash
sudo openssl x509 -in /etc/letsencrypt/live/incubator.lxc-away.site/fullchain.pem \
  -noout -subject -issuer -dates -ext subjectAltName
sudo certbot certificates
```

**已知代价**：`--manual` 签发的证书**无法自动续期**，90 天后需重复一次。
自动化替代方案：在 DNSPod 申请 API Token，改用 `python3-certbot-dns-tencentcloud`，
可全自动验证与续期（推荐在证书验证可用后升级到这条路线）。

### 6.2 方案 B（根治，与 §6.1 并行推进）：完成 ICP 备案

§5.5.1 已确认属**全网未备案拦截**，故备案是唯一的根治手段。

> **❌ R1 更正：原文「把 DNS 解析迁移到 Cloudflare（或阿里云 DNS）以绕开拦截」不成立。**
> §5.5.2 实测证明拦截发生在**腾讯云网络边界**，判定依据是到达该 IP 的 `Host` 头 / TLS `SNI`，
> **与 DNS 由谁托管无关**。把 NS 换成 Cloudflare 后解析结果仍是 `175.24.182.190`，
> 流量照样经过腾讯云边界并携带同样的 Host 头 → 照样被拦。
> （把 Cloudflare 当**反向代理**也不解决：CF 默认向源站转发原始 Host，源站侧仍被拦；
> 靠 Host Header Override 规避属灰色手段，不建议。）
> **真正有效的等价做法只有一个：把源站迁到中国大陆境外的服务器**（届时流量不经过腾讯云边界）。

**推荐路线：ICP 备案**

前置条件（§5.5.3 已核查，**基本全部满足**）：

- ✅ `.site` 后缀已获工信部批复（信管函〔2016〕564号，2016-12-29）
- ✅ 域名在境内注册商 DNSPod（IANA 1697），**无需转移域名**
- ✅ 服务器在腾讯云 → 可直接走腾讯云备案控制台，同一生态最顺
- ✅ 域名在有效期内（至 2027-03-24）
- ⬜ **待使用者确认**：域名实名认证信息是否与拟备案主体（个人/企业）一致

流程与耗时：

```text
1. 腾讯云「ICP 备案」控制台提交主体 + 网站信息
   （需域名实名、身份证件、手机号、真实性核验）
2. 腾讯云初审            约 1~2 个工作日
3. 提交管局审核          各省不同，约 7~20 个工作日
4. 备案通过后            到腾讯云提交「解除拦截」申诉（或等待自动解封）
──────────────────────────────────────────────────
总计约 2~4 周。期间用 §6.1 的 IP 直连方案正常跑，不阻塞任何开发工作。
```

备案完成后的收尾动作：

1. 按 §6.1.9 为 `incubator.lxc-away.site` 签发 Let's Encrypt 证书（**此时才有意义**）
2. Nginx 增加基于域名的 443 server 块，与 IP 块并存
3. 改 `secrets.ini` 的 `ServerUrl` 为域名，重新编译烧录（若 URL 存 NVS 则免烧录）
4. 配置 `certbot renew` 自动续期（90 天证书，比 §6.1 A2 的 6 天稳健得多）

**参考**：[可办理 ICP 备案域名后缀说明（新网）](https://www.xinnet.com/service/cjwt/idc/1589.html) ·
[工信部域名行业管理信息公示](https://domain.miit.gov.cn/) ·
[.SITE 获工信部批复（西部数码）](http://ttt.west263.com/faq/list.asp?unid=1323)

**备选（若不打算备案）**：把整套后端迁到境外 VPS。已完成的服务器侧 40% 工作需重做，
且境内访问境外服务器延迟与稳定性较差；ESP32 出站 POST 一般可行。**不作为首选**。

### 6.3 证书之后的施工顺序

> R1：走 §6.1 A1 路线时，「证书」= 自签 CA 签发的 IP 叶子证书，**当天即可完成**，无需等待备案。
> 下列顺序不变。

```text
1. 在 /etc/nginx/conf.d/incubator.conf 中启用 443 + 反向代理到 127.0.0.1:8000
   （只暴露 Nginx 的 443，FastAPI 绝不监听公网）
2. 在 /opt/incubator/app 建 venv，安装 FastAPI、Uvicorn
3. 建 SQLite（/var/lib/incubator/data.db）与 devices / telemetry / alerts 三张表
4. 实现 §3.3 的接口
5. systemd 托管 FastAPI（重启自启）
6. 部署只读监控网页（纯静态）
7. 最后改造 ESP32 固件并联调
```

---

## 7. 待确认问题（接手方需向使用者确认）

（R1 更新：✅ = 已解决，⬜ = 待确认）

1. ✅ **§4.2 的加热改动**：已确认为 Pi harness 会话按使用者要求所做，**编译已通过**，实机验证待做。
   提交时机：等 SHT30 迁移完工后拆分为独立 commit（见 §4.2 接手建议）。
2. ✅ **§5.2 的域名拦截**：使用者手机 4G 实测跳「未备案」页 → **全网拦截**（§5.5.1）。
   「迁移 DNS 到 Cloudflare」经实测证明**无效**，已从方案中删除（§6.2 R1 更正）。
3. ⬜ **是否授权配置 SSH**（`~/.ssh/config` 增加 `Host incubator-server`），让 AI 直接操作服务器？
   当前未配置，所有服务器命令必须由使用者手动执行并回贴输出。
4. ✅ **手机 4G 访问测试结果**：跳「未备案」页（同 #2）。
5. ⬜ 监控网页是否需要具备历史曲线图？还是先只做「当前状态」页面？
6. ⬜ 设备 API 密钥的生成与下发方式（建议首次配网时写入 NVS）。
7. ⬜ **（R1 新增）并发模型裁决**（§3.4）：接受「Core 0 FreeRTOS 任务 + §3.2 接口不变」，
   还是坚持主循环同步 POST？**此项决定固件联网模块的写法，编码前必须定。**
8. ⬜ **（R1 新增）证书路线**（§6.1）：A1 自签 CA / A2 LE IP 证书 / A3 明文，选哪个？（推荐 A1）
9. ⬜ **（R1 新增）是否启动 ICP 备案**（§6.2）？需先确认域名实名认证信息与备案主体是否一致。
10. ⬜ **（R1 新增）载荷 Schema 4 处缺口**（§3.3.1）：CO₂ 单位 / validity 标志 / `heater_on` / `timestamp`。

---

## 8. 操作规范与红线

### 必须遵守

- 改动 Nginx 前先 `sudo nginx -t`，通过后才 `sudo systemctl reload nginx`。
- 只暴露 Nginx 的 443，FastAPI 只监听 `127.0.0.1:8000`。
- 传感器/执行器/显示逻辑**不得**因联网功能被阻塞或改写。
- 联网失败必须退避重试，不得阻塞主循环，不频繁写 NVS。

### 严禁

- ❌ 触碰 `/root` 下的饥荒游戏服务器文件。
- ❌ 破坏 `/etc/nginx/conf.d/ota.conf` 的 `/fw/` OTA 服务。
- ❌ 修改宝塔目录 `/www/server/panel/vhost/nginx/` 下的配置（改了也不生效）。
- ❌ 使用 `certbot --nginx`。
- ❌ 反复重试失败的 HTTP-01 申请。
- ❌ 把服务器密码、SSH 私钥、腾讯云密钥、Wi-Fi 密码、设备 API 密钥写入仓库或对话。
- ❌ 擅自 `git checkout --` 丢弃工作树中使用者未提交的改动。

---

## 9. 资料位置速查

| 内容 | 位置 |
|---|---|
| 本项目固件源码 | `D:\PlatformIO\Cell\Cell_incubator\src\` |
| 引脚/时序/PID 参数 | `src/Config.h` |
| 全局状态结构（联网数据源） | `src/SystemState.h` |
| 主循环 | `src/main.cpp` |
| Codex 原始会话存档 | `C:\Users\Emila_58035\.codex\sessions\2026\09\15\` 与 `2026\09\16\` |
| DSH 会话存档 | `C:\Users\Emila_58035\.dsh\sessions\--D-PlatformIO-Cell-Cell_incubator--\`（zstd 压缩的 JSONL） |
| 本交接文档 | `HANDOFF.md`（仓库根目录） |
| **结构维护规则（所有 AI 会话必读）** | `AGENTS.md`（仓库根目录）—— 文件所有权分级、禁用命令、提交规范、行尾、自证清单 |
| 远程监控详细设计（R1 已对齐本文档） | `docs/REMOTE_MONITORING.md` |

> 注：`tmp/` 已在 `.gitignore` 中，适合放置临时材料。
> **R3 更新（2026-09-18）**：`HANDOFF.md` 与 `docs/REMOTE_MONITORING.md` 已提交入库（`7762b82` / `f8d6a4f`），
> 不再只有一份本地副本。它们含服务器 IP、域名与 Nginx 布局，**能入库的唯一前提是仓库为 private**；
> 若将来需要转回公开，必须先改写历史抹除这两个文件（见 §4.4）。
> 旧版本文档中“刻意保持未跟踪”的说法已作废；历史提交 `7762b82` 的 diff 仍保留当时的文字，属正常现象。
