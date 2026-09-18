# 远程监控实现设计（固件 + 服务器）

> **本文档从属于 `HANDOFF.md`。凡与 HANDOFF 冲突处，一律以 HANDOFF 为准。**
> 版本：R1 · 2026-09-18 —— 已按 HANDOFF 全面对齐（Nginx / `/api/v1/telemetry` / `devices·telemetry·alerts` /
> Bearer 鉴权 / `secrets.ini` 注入 / `WiFiManager`+`TelemetryClient` 双模块），并纳入 HANDOFF §5.5 的
> **未备案全网拦截 + IP 直连可绕过**实测结论。
> 本文档的价值在于补充 HANDOFF 未展开的**实现级细节**：并发模型论证、模块内部设计、载荷构造、
> Flash/RAM 预算、失效模式、测试与验收标准。
> 状态：**服务器侧已实现并通过本机契约测试**（`server/`，2026-09-18，见 HANDOFF §4.3）；
> **固件侧未编码**，并发模型未裁决（§2）。本文中标记「已实现」的部分以 `server/README.md` 为准。

---

## 0. 与 HANDOFF 的对应关系

| 决策项 | 取值（以 HANDOFF 为准） | HANDOFF 出处 |
|---|---|---|
| 反向代理 / TLS 终结 | **Nginx 1.24.0**（服务器已装），不用 Caddy | §3.1 / §6.3 |
| 后端框架 | **FastAPI + Uvicorn**，只监听 `127.0.0.1:8000` | §3.1 / §6.3 |
| 存储 | **SQLite** `/var/lib/incubator/data.db`，表 `devices` / `telemetry` / `alerts` | §3.1 / §3.3 |
| 前端 | 纯静态只读监控网页，`/var/www/incubator` | §4.3 / §6.3 |
| 不引入 | Docker / InfluxDB / Grafana | §3.1 |
| 访问入口 | **IP 直连 `https://175.24.182.190/`**（域名未备案被全网拦截） | §5.5 / §6.1 |
| 证书 | **自签 CA + IP 叶子证书（SAN=IP）**，固件内嵌 CA | §6.1 A1 |
| API 路径 | `POST /api/v1/telemetry`、`GET /api/v1/devices/{id}/latest`、`/history`、`/api/v1/devices`、`/healthz` | §3.3 |
| 设备鉴权 | `Authorization: Bearer <设备专属密钥>` | §3.3 |
| 固件模块 | `NetworkConfig.h` + `WiFiManager` + `TelemetryClient`（+ 可选 `TimeSync`） | §3.2 |
| 敏感配置 | `secrets.ini` + PlatformIO build_flags 注入，`.gitignore`；仓库留 `secrets.example.ini` | §3.2 |
| 上报周期 | 10 s（传感器维持 温湿度 1 s / CO₂ 5 s） | §3.2 |
| 范围 | **只读**，不做远程控制 | §1.2 |

**本文档新增、HANDOFF 未覆盖的实现约束**（详见对应章节）：

- §2 并发模型：`HTTPClient` 同步阻塞与 HANDOFF §8 红线「不得阻塞主循环」存在实质冲突 → 推荐 Core 0 任务
- §5 Flash/RAM 预算：TLS 预计 +300~500 KB，当前 app 分区余量 964 KB，需实测
- §6 载荷 Schema 4 处缺口（同 HANDOFF §3.3.1）
- §8 失效模式矩阵与「不做本地缓存补传」的取舍
- §9 隔离性验收标准（断网后阀门脉冲时长必须无可测差异）

---

## 1. 目标与边界

### 1.1 要实现

- 培养箱每 10 s 把传感器与控制状态推送到腾讯云服务器
- 服务器持久化，并提供只读网页看板，可从任意网络位置查看
- 支持曲线回看（温度 / 湿度 / CO₂ / 加热功率）与实时状态卡片
- **数据中断可被察觉**（看板显示「离线 / 数据过期」告警）

### 1.2 明确不做（本阶段）

- ❌ 远程修改设定值、远程开关加热/光照/风扇（控制仍靠本机旋钮与按键）
- ❌ 手机 App / 主动推送告警（后续可加 MQTT + Home Assistant 或 Telegram/邮件）
- ❌ 多设备管理后台（协议预留 `device_id`，UI 先按单设备做）
- ❌ 断网期间数据补传（见 §8.2 取舍说明）

### 1.3 硬性约束（源自 HANDOFF §1.2 / §8）

| 约束 | 落地手段 |
|---|---|
| **联网不得影响本地温控** | 阻塞操作全部隔离到 Core 0 任务（§2） |
| **不得阻塞主循环** | `update()` 仅做快照拷贝 + 状态回读，微秒级返回 |
| **不频繁写 NVS** | 不做本地缓存补传；仅沿用 `SettingsStorage` 的手动保存 |
| **凭据不入库** | `secrets.ini` + CA 私钥均 gitignore |
| **公网暴露需鉴权** | 设备 Bearer Token + 看板 Basic Auth（IP 无域名遮蔽，强制项） |
| **不碰既有服务** | 不改 `ota.conf`、不碰 `/root` 饥荒文件、不动宝塔目录 |

---

## 2. 并发模型（编码前必须裁决）

### 2.1 问题

HANDOFF §3.2 要求 `loop()` 中非阻塞调用：

```cpp
wifiManager.update(now);
telemetryClient.update(now, systemState);
```

但 arduino-esp32 的 `HTTPClient` / `WiFiClient` 是**同步阻塞 API**。Wi-Fi 连接可以靠轮询
`WiFi.status()` 做到非阻塞，**HTTP 请求本身不行**。

> **⚠️ 2026-09-18 实测修正**：本节原文估「最坏（connectTimeout + responseTimeout）≈ 3 s」，
> **低了一个数量级**。读框架源码得到：TLS 握手超时默认 **120 s**（`WiFiClientSecure.cpp:40`），
> 而 `HTTPClient::setTimeout()` **根本管不到握手**（它只调 `_client->setTimeout()`，`HTTPClient.cpp:494`），
> 必须另外调 `setHandshakeTimeout()`。即使显式压到 8 s，单次阻塞上限也是 **~14 s**；
> 漏写那一句就是 **~125 s**。完整分阶段表与看门狗分析见 HANDOFF §3.4。

### 2.2 对现有控制逻辑的实测影响

| 受影响模块 | 机制 | 后果 |
|---|---|---|
| `ValveController::pulseValve()` | 靠 `millis() - lastOpenTime >= pulseTime` 关阀 | 主循环卡 3 s → **本该 500 ms 的短脉冲变成 3 s，CO₂ 过量注入** |
| `HeaterController` PID | `PID_v1::Compute()` 按 `SampleTime(1 s)` 节流，积分增量 `ki*error` 绑定 SampleTime 而非真实时差 | 卡 3 s 漏算 2 次积分，控温响应变慢、稳态残差消除变慢 |
| `SHT30Sensor` 采样 | 1 s 周期被推迟 | 控温基于过期读数 |

### 2.3 推荐方案：Core 0 FreeRTOS 任务 + HANDOFF 接口不变

> **保留 HANDOFF §3.2 的模块划分、文件命名、`loop()` 调用形式完全不变**；
> 只把 `TelemetryClient` 内部的阻塞部分放进钉在 **Core 0** 的 FreeRTOS 任务。

依据：ESP32-S3 双核（构建日志 `ESP32S3 240MHz`），Arduino `loopTask` 默认在 **Core 1**，
ESP-IDF Wi-Fi 协议栈任务也在 **Core 0** —— 网络工作放 Core 0 与控制环天然分核，互不抢占。

```
Core 1 (Arduino loopTask)          Core 0 (新增 netTask)
─────────────────────────          ─────────────────────
SHT30 / S8 采样                     WiFi STA 连接与退避重连
PID 控温 / 阀门 / 风扇 / 光照        HTTPS POST（阻塞只发生在这里）
TFT 显示 / 旋钮输入
TelemetryClient::update()  ──portMUX 临界区──▶  TelemetrySnapshot
        ▲                                              │
        └──── volatile 状态回读（wifiOk/cloudOk/rssi）──┘
```

**关键设计**：所有 `SystemState` 的**写入仍然只发生在 Core 1**。
`TelemetryClient::update()` 在 Core 1 上把 volatile 状态回写进 `SystemState` 并按需 `markDisplayDirty()`；
Core 0 任务只读快照、只写自己的 volatile 标志。这样显示与控制路径**完全不需要加锁**，耦合不扩散。

### 2.4 备选方案（不推荐）

按 HANDOFF §3.2 原样在主循环同步 POST，但把超时压到 1 s 内、上报间隔拉到 30 s。
代码最简，但最坏仍卡 ~2 s，阀门脉冲误差不可接受，与 §8 红线冲突。
**仅当双核方案实测遇到障碍时才回退到此路线。**

---

## 3. 固件侧设计

### 3.1 文件清单

| 文件 | 动作 | 说明 |
|---|---|---|
| `secrets.ini` | 新增（**gitignore**） | WiFi SSID/密码、`ServerUrl`、`DeviceToken`、CA 证书 PEM |
| `secrets.example.ini` | 新增（入库） | 模板，占位符不含真实值 |
| `src/NetworkConfig.h` | 新增 | **非敏感**参数：上报周期、超时、任务栈/核/优先级、堆阈值 |
| `src/WiFiManager.h/.cpp` | 新增 | WiFi STA 状态机 + 退避重连（HANDOFF §3.2） |
| `src/TelemetryClient.h/.cpp` | 新增 | 快照采集 + Core 0 任务 + HTTPS POST + Bearer 鉴权 |
| `src/TimeSync.h/.cpp` | 可选 | NTP。**若采纳 §6 建议（服务器打时间戳）则不需要** |
| `src/Config.h` | 修改 | 无需改动（联网常量放 `NetworkConfig.h`，避免污染硬件配置） |
| `src/SystemState.h/.cpp` | 修改 | 增加网络状态字段（供显示层） |
| `src/main.cpp` | 修改 | 实例化 + `begin()` + `update()` |
| `src/DisplayManager.cpp` | 修改（可选，P4） | 页脚加 WiFi/云连接指示 |
| `platformio.ini` | 修改 | 增加 `secrets.ini` 的 build_flags 注入 |
| `.gitignore` | 修改 | 加入 `secrets.ini` |

> `WiFi` / `HTTPClient` / `WiFiClientSecure` 均为 arduino-esp32 内置，**不新增 lib_deps**。

### 3.2 `secrets.ini` 注入方式

```ini
; secrets.ini —— 绝不提交
[secrets]
build_flags =
    -DSECRET_WIFI_SSID=\"YOUR_SSID\"
    -DSECRET_WIFI_PASS=\"YOUR_PASSWORD\"
    -DSECRET_SERVER_URL=\"https://175.24.182.190/api/v1/telemetry\"
    -DSECRET_DEVICE_ID=\"incubator-001\"
    -DSECRET_DEVICE_TOKEN=\"<openssl rand -hex 32>\"
```

`platformio.ini` 中引入（`secrets.ini` 缺失时需有兜底，避免 CI/他人克隆后无法编译）：

```ini
[env:adafruit_feather_esp32s3]
; ...现有配置不变...
extra_configs =
    secrets.ini
build_flags =
    ${secrets.build_flags}
```

CA 证书 PEM 是多行文本，不适合走 `-D` 注入 → **单独放 `src/ServerCaCert.h`（gitignore）**，
仓库留 `src/ServerCaCert.h.example`。

### 3.3 `NetworkConfig.h`（非敏感）

```cpp
#pragma once
#include <stdint.h>

namespace NetCfg {
constexpr uint32_t PublishIntervalMs = 10000;  // 上报周期（HANDOFF §3.2）
constexpr uint32_t WifiRetryMs       = 5000;   // WiFi 退避重连间隔
constexpr uint32_t WifiRetryBackoffMaxMs = 60000;  // 退避上限，避免长期断网时高频重试
constexpr uint16_t  ConnectTimeoutMs = 3000;
constexpr uint16_t  ResponseTimeoutMs= 3000;

constexpr uint8_t   TaskCore         = 0;      // 与控制环（Core 1）分核
constexpr uint32_t  TaskStackBytes   = 12288;  // TLS 握手需要较大栈
constexpr uint8_t   TaskPriority     = 2;
constexpr uint32_t  MinFreeHeap      = 40960;  // 低于此值跳过本轮上报，避免 OOM 重启

constexpr char      FirmwareVersion[] = "0.1.0";
}
```

### 3.4 跨核快照结构

网络任务只读，不写 `SystemState`。用 POD 结构在 `portMUX` 临界区内完成拷贝：

```cpp
struct TelemetrySnapshot {
  float    temperature, humidity;
  uint32_t co2;                       // 设备单位（S8 寄存器 × Co2ScaleFactor），不换算，见 §6
  bool     temperatureValid, humidityValid, co2Valid;
  float    setpoint;
  bool     heatingEnabled;            // 加热开关（见 §6 缺口 3）
  double   heaterOutput;
  uint32_t heaterDuty;                // 0..255
  bool     lightEnabled;
  uint8_t  lightBrightness;           // 0..100
  bool     fanEnabled;
  bool     valveOpen;
  uint32_t uptimeS;
};
```

临界区内**只做字段赋值**，禁止任何 `Serial` 输出。耗时约几微秒。

### 3.5 `WiFiManager` 接口

```cpp
class WiFiManager {
 public:
  void begin();
  void update(uint32_t now);          // Core 1 调用，非阻塞：仅推进重连状态机
  bool isConnected() const;
  int8_t rssi() const;

 private:
  bool     connected_ = false;
  uint32_t nextRetryAt_ = 0;
  uint32_t backoffMs_ = NetCfg::WifiRetryMs;
};
```

要点：
- `WiFi.mode(WIFI_STA)` + `WiFi.setSleep(false)`（市电供电，关 modem sleep 提升可靠性与响应）
- `WiFi.setAutoReconnect(true)`，但**不调用 `waitForConnectResult()`**（会阻塞数秒）
- 靠轮询 `WiFi.status()` 判定；断开时按 `backoffMs_` 指数退避，封顶 60 s
- **仅在状态跳变时打印日志**，避免淹没 AHT/S8 的诊断输出

### 3.6 `TelemetryClient` 接口

```cpp
class TelemetryClient {
 public:
  bool begin(const SystemState &state);              // 建 Core 0 任务
  void update(uint32_t now, SystemState &state);     // Core 1：取快照 + 回读状态

 private:
  static void taskEntry(void *arg);
  void run();                                        // Core 0 任务体
  bool publish(const TelemetrySnapshot &snap);       // HTTPS POST
  size_t buildJson(char *buf, size_t cap, const TelemetrySnapshot &snap);

  TaskHandle_t task_ = nullptr;
  portMUX_TYPE mux_  = portMUX_INITIALIZER_UNLOCKED;
  TelemetrySnapshot snapshot_{};

  volatile bool     wifiOk_    = false;
  volatile bool     cloudOk_   = false;
  volatile int16_t  lastHttp_  = 0;
  volatile uint32_t lastPubMs_ = 0;
  volatile int8_t   rssi_      = 0;
};
```

Core 0 任务体（伪代码）：

```
run():
  WiFiClientSecure client; client.setCACert(kServerCaPem); client.setHandshakeTimeout(8);
  HTTPClient http; http.setReuse(true);              // TLS 会话复用，握手一次
  http.begin(client, SECRET_SERVER_URL);
  http.setConnectTimeout(NetCfg::ConnectTimeoutMs);
  http.setTimeout(NetCfg::ResponseTimeoutMs);

  forever:
    now = millis()
    if !WiFi.isConnected():  wifiOk_=false; cloudOk_=false; vTaskDelay(100ms); continue
    wifiOk_ = true; rssi_ = WiFi.rssi()

    if now - lastPubMs_ >= PublishIntervalMs:
        lastPubMs_ = now
        if ESP.getFreeHeap() < MinFreeHeap: logOnce("skip: low heap"); vTaskDelay(20ms); continue
        临界区取 snapshot_
        code = publish(snapshot_)
        cloudOk_ = (code >= 200 && code < 300); lastHttp_ = code
        if !cloudOk_: http.end()      // 断开复用连接，下轮重新 begin
    vTaskDelay(pdMS_TO_TICKS(20))     // 必须规律让出 CPU，避免任务看门狗
```

### 3.7 `SystemState` 新增字段

```cpp
  // 网络状态（由 TelemetryClient::update 在 Core 1 写入）
  bool     wifiConnected;
  bool     cloudReachable;
  int8_t   wifiRssi;
  int16_t  netLastHttpCode;
  uint32_t netLastPublishMs;   // millis() 时间戳，供显示「最近上报」
```

构造初值全为 `false / 0`。

### 3.8 `main.cpp` 接入

```cpp
WiFiManager     wifiManager;
TelemetryClient telemetryClient;

void setup() {
  // ...现有初始化顺序完全不变...
  wifiManager.begin();
  telemetryClient.begin(systemState);   // 放最后，确保其余模块已就绪
}

void loop() {
  const uint32_t now = millis();
  // ...现有调用顺序完全不变...
  fanController.update(systemState);

  wifiManager.update(now);                    // ← HANDOFF §3.2 原样
  telemetryClient.update(now, systemState);   // ← HANDOFF §3.2 原样，微秒级返回
}
```

**接入原则**：追加在现有调用链末尾，不插入中间，避免影响现有模块的相对时序。

### 3.9 资源预算

| 项目 | 当前（SHT30 迁移前实测） | 预估新增 | 评估 |
|---|---|---|---|
| Flash | 477 KB / 1.44 MB（33.1%） | WiFi + TLS 协议栈约 **+300~500 KB** | 预计 ~800–980 KB，**可容纳但必须实测确认** |
| RAM | 39.5 KB / 320 KB（12.1%） | netTask 栈 12 KB + TLS 缓冲 ~40 KB + WiFi 驱动 ~30 KB | 预计 ~120 KB，余量充足 |

⚠️ **Flash 是主要风险点**（分区表 `tinyuf2-partitions-4MB.csv`，app 分区约 1.4 MB）。
建议 P1 先跑**明文 HTTP** 量出基线，P2 再加 TLS。若超限的裁剪手段：
`build_flags = -Os`、裁剪 mbedtls cipher suite、关闭 WiFi 扫描冗余。

> 注：SHT30 迁移（`robtillaart/SHT85` 库）会改变上述基线，迁移完成后需重新实测。

---

## 4. 通信协议

### 4.1 上行 `POST /api/v1/telemetry`

**请求头**
```
Content-Type: application/json
Authorization: Bearer <设备专属密钥>
```

**请求体**（约 320 字节，扁平结构，`snprintf` 手搓，**不引入 ArduinoJson**）

```json
{
  "device_id": "incubator-001",
  "uptime_s": 183420,
  "firmware_version": "0.1.0",

  "temperature": 37.02,  "temperature_ok": true,
  "humidity": 55.3,      "humidity_ok": true,
  "co2": 51000,          "co2_ok": true,

  "target_temperature": 37.0,
  "heater_on": true,     "heater_pwm": 128,
  "light": false,        "light_pct": 0,
  "fan": true,
  "valve": false,

  "rssi": -57,           "free_heap": 210000
}
```

字段与 `SystemState` 的映射见 §6。

**响应**
- `204 No Content` — 成功（设备不解析响应体，省内存）
- `401` — Token 错误（打印一次告警并降频重试，避免无效流量）
- `400` — 格式错误 / 校验失败
- `429` — 触发限流

设备侧只关心「是否 2xx」，非 2xx 记入 `netLastHttpCode` 供串口与显示诊断。

### 4.2 下行：仅看板查询（浏览器 ↔ 服务器）

设备**不接收任何下行指令**。看板数据全部来自服务器：
`GET /api/v1/devices`、`GET /api/v1/devices/{id}/latest`、`GET /api/v1/devices/{id}/history`、`GET /healthz`。

---

## 5. 服务器侧设计

### 5.1 目录与部署形态

```
/opt/incubator/app/            ← venv + FastAPI 代码（属 incubator 用户）
  app.py            路由：ingest / latest / history / devices / healthz
  models.py         Pydantic 校验模型
  db.py             SQLite 连接（WAL）与查询
  schema.sql        建表语句
  retention.py      每日清理 + 小时聚合（P3）
  static/index.html 只读看板（Chart.js CDN）
/var/lib/incubator/data.db     ← SQLite（750，属 incubator）
/var/www/incubator/            ← 静态页（可选，也可由 FastAPI 直接托管）
/etc/incubator/                ← 设备 Token 等配置（750，root:incubator）
/etc/nginx/conf.d/incubator.conf  ← 443 + 反代，与 ota.conf 并存
/etc/systemd/system/incubator-api.service
```

### 5.2 数据库 Schema

```sql
PRAGMA journal_mode = WAL;

CREATE TABLE IF NOT EXISTS devices (
  device_id   TEXT PRIMARY KEY,
  label       TEXT,
  -- 注意：api_token 不在本表（2026-09-18 裁决）。Token 存在 /etc/incubator/devices.json，
  -- 因为数据库是进备份、进查询的那个东西，不该存放凭据。label 则不入 devices.json 的强制项。
  first_seen  INTEGER,
  last_seen   INTEGER,
  fw_version  TEXT
);

CREATE TABLE IF NOT EXISTS telemetry (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id   TEXT NOT NULL,
  recv_ts     INTEGER NOT NULL,     -- 服务器 Unix 秒（权威时间，见 §6 缺口 4）
  uptime_s    INTEGER,
  fw_version  TEXT,

  temperature REAL,   temperature_ok INTEGER,
  humidity    REAL,   humidity_ok    INTEGER,
  co2         INTEGER, co2_ok        INTEGER, co2_available INTEGER,   -- 整数 ppm

  target_temperature REAL,
  heater_on   INTEGER, heater_pwm INTEGER,
  light       INTEGER, light_pct  INTEGER,
  fan         INTEGER,
  valve       INTEGER,

  rssi        INTEGER, free_heap INTEGER
);
CREATE INDEX IF NOT EXISTS idx_telemetry_dev_ts ON telemetry(device_id, recv_ts DESC);

CREATE TABLE IF NOT EXISTS alerts (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id   TEXT NOT NULL,
  raised_ts   INTEGER NOT NULL,
  cleared_ts  INTEGER,
  kind        TEXT NOT NULL,        -- 'offline' | 'temp_out_of_range' | 'sensor_fault'
  detail      TEXT
);
```

> **以 `server/schema.sql` 为准**（已实现并测试）。相对上面的草图，实际建表多出三列：
> `co2_available`、`heap_min`、`heap_int`；另有 `idx_alerts_open` 部分索引用于查未恢复告警。

**数据量测算**（10 s 间隔，单设备）

| 周期 | 行数 | 估算体积 |
|---|---|---|
| 1 天 | 8,640 | ~1 MB |
| 1 月 | 26 万 | ~30 MB |
| 1 年 | 315 万 | ~350 MB |

服务器磁盘仅剩 ~8.3 GB（HANDOFF §2.3）→ **必须配留存策略**：
原始数据保留 90 天（约 78 万行、~90 MB），`retention.py` 每日清理并聚合进小时表。
另建议按 HANDOFF §2.3 执行 `sudo journalctl --vacuum-size=500M` 先释放 ~1.5 GB。

### 5.3 API 与校验

| 方法 | 路径 | 鉴权 |
|---|---|---|
| POST | `/api/v1/telemetry` | **Bearer Token**（常量时间比较） |
| GET | `/api/v1/devices` | Basic Auth |
| GET | `/api/v1/devices/{id}/latest` | Basic Auth |
| GET | `/api/v1/devices/{id}/history?hours=&step=` | Basic Auth |
| GET | `/healthz` | 无 |
| GET | `/` | Basic Auth |

**Pydantic 校验范围**（拒绝脏数据与恶意超长载荷）：
`temperature` ∈ [-40, 85]、`humidity` ∈ [0, 100]、`co2` ∈ [0, 200000]、
`heater_pwm` ∈ [0, 255]、`light_pct` ∈ [0, 100]、`device_id` 长度 ≤ 64。

**限流**：POST 端点按 IP 20 req/min（正常 6 req/min）。

**降采样**：`history` 区间 > 6 h 时按 1 分钟桶返回均值，避免一次吐 8640 点拖垮浏览器。

### 5.4 Nginx 配置要点

```nginx
# /etc/nginx/conf.d/incubator.conf —— 与 ota.conf 并存，不改 ota.conf
server {
    listen 443 ssl;
    server_name 175.24.182.190;            # IP 直连，见 HANDOFF §5.5.2

    ssl_certificate     /etc/incubator/certs/server.pem;   # 自签，SAN=IP
    ssl_certificate_key /etc/incubator/certs/server.key;
    ssl_protocols       TLSv1.2 TLSv1.3;

    # 看板：Basic Auth（IP 无域名遮蔽，强制项）
    location / {
        auth_basic "Incubator";
        auth_basic_user_file /etc/incubator/htpasswd;
        proxy_pass http://127.0.0.1:8000;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }

    # 设备上报：不加 Basic Auth（设备用自带 Bearer Token）
    location /api/v1/telemetry {
        auth_basic off;
        client_max_body_size 4k;
        limit_req zone=telemetry burst=10 nodelay;
        proxy_pass http://127.0.0.1:8000;
        proxy_set_header Host $host;
        proxy_set_header Authorization $http_authorization;
    }
}
```

红线复述（HANDOFF §8）：改 Nginx 前先 `sudo nginx -t`；**不用 `certbot --nginx`**；
FastAPI 只监听 `127.0.0.1:8000`；不动宝塔目录；不破坏 `ota.conf` 的 `/fw/`。

### 5.5 看板设计

```
┌──────────────────────────────────────────────────────┐
│  🟢 在线 · 最近上报 3s 前 · WiFi -57dBm · fw 0.1.0     │ ← 状态条（红=离线）
├──────────────────────────────────────────────────────┤
│   温度 37.02 ℃   │  湿度 55.3 %  │  CO₂ 5.10 %        │ ← 大数字卡片
│   设定 37.0 ℃    │   OK          │  OK                │   (co2_ppm/10000)
├──────────────────────────────────────────────────────┤
│  [ 1h ][ 6h ][ 24h ][ 7d ]                           │
│  ╭────────────────────────────────────────────────╮  │
│  │ 温度曲线（含 setpoint 虚线）+ 加热功率 % 副轴     │  │ ← Chart.js
│  ╰────────────────────────────────────────────────╯  │
│  ╭──────────────────╮ ╭──────────────────────────╮   │
│  │ 湿度曲线          │ │ CO₂ 曲线 + 阀门开标记      │   │
│  ╰──────────────────╯ ╰──────────────────────────╯   │
│  加热 ON · PWM 50% │ 光照 OFF │ 风扇 ON │ 阀 CLOSED    │ ← 执行器状态
└──────────────────────────────────────────────────────┘
```

刷新：前端每 10 s `fetch('/api/v1/devices/{id}/latest')`，按需拉 `history`。轮询而非 WebSocket。

**离线判定（对培养箱至关重要）**：
`now - recv_ts > 3 × 上报间隔（30 s）` → 状态条变红显示 `OFFLINE / 数据已过期 Xs`，并写入 `alerts` 表。
这样「断电、断网、固件死机」三种情况都能被发现，而不是看到一条静止曲线误以为一切正常。

---

## 6. 载荷 Schema 的 4 处修正（同 HANDOFF §3.3.1）

对照 `src/SystemState.h` 实际字段，HANDOFF §3.3 的 JSON 示例需修正：

| # | 问题 | 现状 | 采纳方案 |
|---|---|---|---|
| 1 | **CO₂ 单位不一致** | 示例 `"co2": 5.1` 是百分比 float；固件 `SystemState.co2` 是 `S8 寄存器原值 × Config::Co2ScaleFactor(=10)`，TFT 又按 `"ppm"` 标签显示（真实物理单位**未确认**） | **上报 `SystemState.co2` 原值，服务器与看板都不换算**（2026-09-18 使用者裁决）。仍实施中抓到的 10 倍偏差风险见 HANDOFF §3.3.1；**不实现 CO₂ 阈值告警**直到单位被硬件确认 |
| 2 | **缺有效性标志** | 无 `temperatureValid`/`humidityValid`/`co2Valid` | **必须补** `*_ok`。否则看板无法区分「传感器故障」与「读数恰为 0」，HANDOFF §4.2 加热解耦改动的价值在远程端完全体现不出来 |
| 3 | **缺加热开关状态** | §3.2 已把 `heatingEnabled` 列为数据源，但 §3.3 载荷只有 `heater_pwm` | **必须补** `heater_on`。开关 ON + 传感器故障时 `heater_pwm=0`，与「用户主动关加热」的 0 含义完全不同 |
| 4 | **`timestamp` 需设备对时** | §3.2 把 NTP 列为「可选」，但载荷含 `timestamp`；不启用 NTP 则无从填写 | 设备只报 **`uptime_s`**，服务器用 `recv_ts` 打权威时间戳。**可不实现 `TimeSync` 模块**，少一个故障点 |

新增可选字段：`rssi`、`free_heap`（远程排查内存泄漏）。

`SystemState` → 载荷字段映射：

```
temperature      → temperature        temperatureValid → temperature_ok
humidity         → humidity           humidityValid    → humidity_ok
co2              → co2 (设备单位，不换算)   co2_ok → co2_ok，co2Available → co2_available
setpoint         → target_temperature
heatingEnabled   → heater_on          heaterDuty       → heater_pwm
lightEnabled     → light              lightBrightness  → light_pct
fanEnabled       → fan                valveOpen        → valve
(millis()/1000)  → uptime_s           ESP.getFreeHeap()→ heap_free
                                        ESP.getMinFreeHeap() → heap_min
                                        heap_caps_get_free_size(INTERNAL|8BIT) → heap_int
WiFi.RSSI()      → rssi               NetCfg::FirmwareVersion → firmware_version
```

> `heaterOutput`（PID 原始输出 0~255 double）与 `heaterDuty`（实际写入的整数占空）二者上报
> `heaterDuty` 即可；看板换算功率百分比 = `heater_pwm / 255 * 100`。

---

## 7. 安全（IP 直连场景下要求更高）

IP 无域名遮蔽、全网扫描即可发现，且本机 80 端口已公开提供 `/fw/` OTA。因此：

- [x] 全站 HTTPS（自签 CA，固件内嵌 CA PEM 用 `setCACert()`）
- [x] 设备上报：`Authorization: Bearer <key>`，服务端 **`hmac.compare_digest()` 常量时间比较**
- [x] **看板：Basic Auth 强制**（`htpasswd`，否则任何人都能看到培养箱数据）
- [x] FastAPI 只绑 `127.0.0.1`；SQLite 与 CA 私钥文件权限 600/640，属主 `incubator`
- [x] POST 端点 `limit_req` + `client_max_body_size 4k`
- [x] Pydantic 范围校验 + 字段限长
- [x] `secrets.ini` / `src/ServerCaCert.h` / CA 私钥 **全部 gitignore**，仓库只留 `.example`
- [x] **CA 私钥绝不进固件、绝不进仓库**（固件只需 CA 公钥证书）
- [ ] 备案完成后：切回域名 + Let's Encrypt 90 天证书 + `certbot renew` 自动续期（HANDOFF §6.2）

**Token 生成与下发**（HANDOFF §7.6 待确认项的建议）：
`openssl rand -hex 32` 生成 → 写入服务器 `/etc/incubator/devices.json` → 写入开发机 `secrets.ini` → 编译烧录。
若后续要支持多设备/现场配网，再改为首次配网写入 NVS。

---

## 8. 可靠性设计

### 8.1 失效模式矩阵

| 故障 | 设备侧行为 | 用户可见结果 |
|---|---|---|
| WiFi 断开 | 指数退避重连（5 s → 60 s 封顶）；**控制环完全不受影响** | 看板 OFFLINE；TFT 可选显示断网图标 |
| WiFi 恢复 | 自动重连，下一周期恢复上报 | 曲线出现一段缺口后恢复 |
| DNS / 服务器不可达 | 记录 HTTP 错误码，下周期重试，**不加速重试** | 看板 OFFLINE |
| TLS 握手失败 | 同上；串口打印错误码（状态跳变时才打） | 看板 OFFLINE |
| Token 错误 (401) | 打印一次告警并**降频重试** | 看板无数据 |
| 堆内存不足 (<40 KB) | 跳过本轮上报并打印，不冒险触发 OOM 重启 | 偶发缺口 |
| 服务器 5xx | 下周期重试，数据丢弃 | 短暂缺口 |
| 服务器磁盘满 | 服务器侧 500；设备同上 | 看板报错（需服务器监控） |

### 8.2 明确的取舍

- **不做本地缓存补传**：断网期间数据直接丢弃。对培养箱监控而言「知道断过网」比「补齐历史」更重要，
  且省去 flash 写入与补传状态机 —— 同时满足 HANDOFF §1.2「不频繁写 NVS（避免闪存磨损）」。
  若后续确需，可加环形缓冲（1 小时 = 360 条 ≈ 100 KB）到 LittleFS。
- **不做 QoS 保证**：HTTP 尽力而为，符合只读监控定位。
- **网络故障永不升级**：任何网络异常都不触碰 `heatingEnabled`、`setpoint`、阀门等控制状态。硬性隔离原则。

### 8.3 日志规范

沿用现有 `[MODULE]` 前缀风格，新增 `[NET]` / `[WIFI]`：

```
[WIFI] Connecting to SSID...
[WIFI] Connected, IP=192.168.1.50, RSSI=-57
[WIFI] Lost; retry in 5 s
[NET]  POST ok (204), 312 bytes, 143 ms
[NET]  POST failed: -1 (connection refused)
[NET]  Low heap 38 KB, skipping publish
```

**只在状态跳变时打印**，避免每 10 s 刷屏淹没 SHT30 / S8 的诊断输出。

---

## 9. 分阶段实施计划

### P1 · 局域网链路打通（明文 HTTP，不含 TLS）
- [ ] `secrets.ini` / `secrets.example.ini` / `.gitignore` / `platformio.ini` 注入
- [ ] `src/NetworkConfig.h`、`src/ServerCaCert.h.example`
- [ ] `SystemState` 新增网络字段
- [ ] `WiFiManager`、`TelemetryClient`（Core 0 任务 + 快照 + 明文 POST）
- [ ] `main.cpp` 接入
- [x] `server/` 实现：FastAPI + SQLite 三表 + 只读看板（2026-09-18 完成、45 项契约测试本机通过，见 HANDOFF §4.3）
- [x] 测试：`server/tests/test_api_smoke.py`（标准库 + 真实 uvicorn 子进程，非 in-process）
- [ ] 笔记本跑服务器，指向局域网地址验证
- [ ] **Flash / RAM 实测**（联网模块加入后的新基线）。SHT30 迁移后的当前基线已实测：
      Flash 477,781 B / 1,441,792 B = **33.1%**，RAM 39,548 B / 327,680 B = **12.1%**（2026-09-18，`pio run`）。
      注：本板有 2 MB PSRAM 且 `CONFIG_SPIRAM_USE_MALLOC 1`，>4 KB 的分配（如 TLS 缓冲）会优先落 PSRAM，
      所以内部 RAM 压力比本文 §3.9 的估算宽松，**Flash 才是真瓶颈**（app 分区只有 1.41 MB）
- **验收**：看板出现实时曲线；**拔掉路由器后温度曲线与阀门脉冲时序与拔网前无可测差异**

### P2 · 上云 + 安全加固
- [ ] 服务器自签 CA + IP 叶子证书（HANDOFF §6.1 A1）
- [ ] Nginx 443 + 反代 + Basic Auth + `limit_req`（**先 `nginx -t`，不碰 `ota.conf`**）
- [ ] `WiFiClientSecure` + 内嵌 CA
- [ ] Bearer Token 鉴权 + Pydantic 校验
- [ ] systemd 托管 FastAPI
- [ ] `ServerUrl` 改指 `https://175.24.182.190/api/v1/telemetry`
- **验收**：手机 4G 下能打开看板；错误 Token 被 401 拒绝；抓包无明文

### P3 · 看板完善与数据留存
- [ ] Chart.js 多曲线 + 时间范围切换（1h/6h/24h/7d）
- [ ] 服务端降采样（>6 h 用 1 min 桶）
- [ ] 离线告警视觉强化 + 写入 `alerts` 表
- [ ] `retention.py`：90 天清理 + 小时聚合
- [ ] 执行器状态区
- [ ] `journalctl --vacuum-size=500M` 释放磁盘（HANDOFF §2.3）

### P4 · 可选增强
- [ ] TFT 页脚 WiFi/云连接图标（本机也能看出断网）
- [ ] ICP 备案通过后切回域名 + LE 90 天证书自动续期（HANDOFF §6.2）
- [ ] 阈值越界告警（服务器规则 → 邮件 / Telegram / Bark）
- [ ] 本地环形缓冲补传
- [ ] `TimeSync`（NTP）——仅当确需设备端时间戳
- [ ] 若未来要远程控制：另起独立设计（指令队列、鉴权分级、冲突处理、审计日志），**不在本方案范围**

---

## 10. 测试与验收要点

### 10.1 功能测试

| 用例 | 预期 |
|---|---|
| 正常上电 | 10 s 内看板出现首条数据，之后每 10 s 一条 |
| 修改 setpoint | 看板 `target_temperature` 随之变化 |
| 加热运行中 | `heater_pwm` 随 PID 输出变化，曲线平滑 |
| 拔掉 SHT30 | `temperature_ok=false`、`temperature=null`；**加热暂停但 `heater_on` 仍为 true**（验证 §4.2 解耦逻辑） |
| CO₂ 传感器故障 | `co2_ok=false`，阀门逻辑正常关闭 |
| 用户关掉加热开关 | `heater_on=false` 且 `heater_pwm=0` |

### 10.2 隔离性测试（**最重要，对应 HANDOFF §8 红线**）

| 用例 | 预期 |
|---|---|
| 关闭路由器 30 分钟 | 温度控制曲线、**阀门脉冲时长**与联网时**无可测差异**；串口无刷屏 |
| kill 掉服务器进程 | 同上；设备仅打印 POST 失败，不重启 |
| 填入不存在的服务器地址 | 同上，控温不受影响 |
| 连续运行 24 h | `free_heap` 字段**不持续下降**（无内存泄漏） |

### 10.3 性能核对

- [ ] 上报期间 `HeaterController` 的 PID 计算间隔仍稳定 ~1 s
- [ ] 上报期间 `ValveController` 脉冲时长误差 **< 50 ms**
- [ ] `telemetryClient.update()` 在 Core 1 的耗时 **< 100 µs**（用 `micros()` 打点验证）

---

## 11. 已否决的备选方案

| 方案 | 否决理由 |
|---|---|
| **Caddy 自动 TLS** | 服务器已装 Nginx 1.24.0 且承载 `/fw/` OTA，引入第二个反代增加冲突风险（HANDOFF §3.1 明确复用 Nginx） |
| **ESP32 内置 Web 服务器** | 设备做服务端，公网访问需端口映射；且请求处理会阻塞主循环，违反 §8 红线 |
| **WebSocket 长连接** | 需维护心跳与重连状态机，只读场景收益低；轮询已足够 |
| **MQTT + Home Assistant** | 服务器端零代码是优点，但需部署 HA（较重）。**若后续想要手机推送告警，这是最佳补充路径**，可与本方案并存 |
| **云平台（Adafruit IO / ThingsBoard）** | 实验数据托管第三方；定制看板受限 |
| **服务器主动轮询设备** | NAT 后不可达，直接排除 |
| **InfluxDB + Grafana** | HANDOFF §3.1 明确不引入；单设备场景 SQLite + 自绘看板更简洁 |
| **Let's Encrypt IP 证书（§6.1 A2）** | 6 天有效期 + certbot ≥5.4 + 无 nginx 安装器支持 → 续期链条脆弱，对无人值守设备风险偏高。保留为「无法接受浏览器警告」时的备选 |
| **迁移 DNS 到 Cloudflare 以绕开拦截** | **实测无效**：拦截在腾讯云边界按 Host/SNI 判定，与 DNS 托管方无关（HANDOFF §6.2 R1 更正） |

---

## 12. 待确认事项

与 HANDOFF §7 保持一致，实施前需确认：

1. ⬜ **并发模型**（§2）：接受「Core 0 任务 + HANDOFF §3.2 接口不变」，还是坚持主循环同步 POST？**编码前必须定**；
   2026-09-18 使用者决定固件侧押后，此项仍悬空
2. ✅ **证书路线已定**：A1 自签 CA，**只签 IP 叶子证书**（`SAN=IP:175.24.182.190`），不用域名
3. ✅ **ICP 备案降级为可选**：IP 直连已绕开拦截，备案只为消除浏览器证书警告
4. ✅ **载荷 Schema**：4 处修正全采纳，另加 `co2_available` 与 `heap_free`/`heap_min`/`heap_int`（已实现）
5. ✅ **SSH 授权**：继续**手动模式**，文件走宝塔面板（`0.0.0.0:28095`）上传 zip
6. ✅ **看板历史曲线已实现**（手写 canvas、零外部依赖，1h/6h/24h/7d）
7. ⬜ **设备 API 密钥的固件侧下发方式**：`secrets.ini` 编译期注入，还是首次配网写 NVS
   （服务器侧已定：`openssl rand -hex 32` → `/etc/incubator/devices.json`，不入库、不打印）
8. ✅ SHT30 迁移早已完成，基线已重测（见 §3.9 与 §9 P1）
