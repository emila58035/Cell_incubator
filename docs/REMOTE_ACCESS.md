# 远程访问功能 · 开发方案与进度（独立参考文档）

> **独立性声明**：本文档独立维护，**不引用、不依赖**仓库内任何其它文档。
> 本文所写的一切均以**当前代码与实测结果**为准；事实变化时**只改本文**，不需要同步到别处。
>
> 覆盖范围：培养箱远程监控功能的两端 —— **服务器端**（已上线）与**单片机端**（未开工）。
> 最后更新：**2026-09-18 23:55**（服务器端部署与验收完成当天）。

---

## 0. 状态速览

| 部分 | 状态 | 证据 |
|---|---|---|
| 服务器端后端 | ✅ **已上线** | systemd `active`、只绑 `127.0.0.1:8000`、SQLite 三表 WAL |
| HTTPS 入口 | ✅ **已上线** | `https://175.24.182.190/` 可访问，自签 IP 证书，Basic Auth 生效 |
| 只读看板 | ✅ **已上线** | 浏览器输入密码可正常打开（已于 2026-09-18 人工验收） |
| 数据链路 | ⬜ **空管道** | 看板显示"尚无设备上报"，因为**没有任何设备在上报** |
| 单片机端（温控本体） | ✅ 已开发完成 | `pio run` SUCCESS；Flash 477,781 B / 1,441,792 B（33.1%），RAM 39,548 B / 327,680 B（12.1%） |
| 单片机端（联网模块） | ⬜ **未编码** | 仅有方案（§6）；**开工前有一个必须先裁决的设计项**（§6.3） |
| 端到端联调 | ⬜ 未开始 | 依赖单片机端联网模块 |

**唯一阻塞**：单片机端联网的**并发模型未裁决**（§6.3）。服务器端不需要再等人。

---

## 1. 目标与边界

### 1.1 要实现

- 培养箱每 **10 秒**把传感器与控制状态推送到自建服务器
- 服务器持久化并提供**只读**网页看板，可从任意网络位置查看
- 曲线回看（温度 / 湿度 / CO₂ / 加热功率）与实时状态卡片
- **数据中断可被察觉**：看板显示"离线 / 数据已过期"

### 1.2 明确不做（当前阶段）

- ❌ 远程修改设定值、远程开关加热 / 光照 / 风扇（控制仍靠本机旋钮与按键）
- ❌ 手机 App / 主动推送告警
- ❌ 多设备管理后台（协议已预留 `device_id`，界面按单设备做）
- ❌ 断网期间数据补传（见 §6.6 取舍）
- ❌ 引入 Docker / InfluxDB / Grafana / 第三方云平台

### 1.3 硬性约束

| 约束 | 落地手段 |
|---|---|
| **联网不得影响本地温控** | 所有阻塞操作隔离到独立 RTOS 任务（§6.3） |
| **不得阻塞主循环** | 主循环里的联网调用只做快照拷贝 + 状态回读，微秒级返回 |
| **不频繁写 NVS** | 不做本地缓存补传，仅沿用既有的手动保存设置 |
| **凭据不入库、不入仓库** | 设备 Token 与 CA 私钥只存在于服务器文件系统；配置模板用 `.example` |
| **公网暴露必须有鉴权** | 设备用 Bearer Token；看板用 Basic Auth；上报端点限流 |

---

## 2. 环境与访问事实

### 2.1 服务器

```
公网 IP：       175.24.182.190
系统：          Ubuntu 24.04.4 LTS，kernel 6.8.0-111-generic
Nginx：         1.24.0（Ubuntu 包）
Python：        3.12.3（python3-venv 可用）
SQLite：        3.45.1        OpenSSL：3.0.13        certbot：2.9.0
磁盘：          39 G 总量 / 30 G 已用 / **8.0 G 可用**（79%）
内存：          3.7 G（可用约 2.7 G，swap 已用约 627 M）
登录方式：      SSH 用户 `ubuntu`，`sudo` 可用
```

服务器同时是一个**长期运行的桌面环境**，上面还有与本项目无关的服务（VNC 5901/6001、面板 28095、80 端口的固件 OTA）。**除本项目的 Nginx 配置文件外，不得改动其它任何服务。**

### 2.2 端口与防火墙

| 端口 | 状态 | 说明 |
|---|---|---|
| 22 | 开放 | SSH |
| 80 | 开放 | 既有 OTA 服务（`/fw/`）与本项目无关，**必须保持可用** |
| **443** | **开放** | **本项目 HTTPS 入口** |
| 8000 | **被安全组丢弃** | FastAPI 只绑 `127.0.0.1`，公网探测为 `TimedOut`（正确） |
| 28095 / 5901 / 6001 | 开放 | 面板与 VNC，**与本项目无关** |

443 在部署前实测为 `ConnectionRefused`（说明腾讯云安全组**已放行**该端口、只是没有监听者），因此**不需要**去云控制台加规则。对照：9999 / 8443 实测为 `TimedOut`（未放行）。

### 2.3 域名与备案：为什么直接用 IP

- 域名 `incubator.lxc-away.site` 解析正确（指向本机 IP），但**未完成 ICP 备案**。
- 该域名在腾讯云网络边界被**按 HTTP `Host` 头 / TLS `SNI` 中的域名字符串**拦截 → 返回 302 跳转到"未备案"提示页。
- 拦截**只认域名**：用 IP 直连（`Host` 不含域名）返回 200。**手机 4G 实测域名跳未备案页，IP 不受影响。**
- 因此本项目**不使用域名**：设备与浏览器都直接访问 IP，证书签在 IP 上。
- 顺带消除的故障点：DNS 解析、DNS TTL、Let's Encrypt 的 HTTP-01 校验（海外校验节点同样被拦）、90 天续期。
- 备案属于**可选优化**（只为消除浏览器的证书警告），**不是前置条件**。

### 2.4 凭据位置（值是秘密，本文不记录）

| 用途 | 位置 | 权限 |
|---|---|---|
| 设备上报 Token | `/etc/incubator/devices.json` | `0640 root:incubator` |
| 设备上报 Token 的**格式** | `{"<device_id>": {"token": "<64位hex>", "label": "<显示名>"}}` | — |
| 看板 Basic Auth | `/etc/nginx/incubator.htpasswd` | `0640 root:www-data`（**必须让 nginx worker 能读**） |
| 自签 CA 私钥 | `/etc/incubator/certs/incubator-ca.key` | `0600 root:root`（**必须离线备份**） |
| 自签 CA 证书 | `/etc/incubator/certs/incubator-ca.pem` | `0644`（这是给固件内嵌的**公钥**） |
| 服务器叶子证书 / 私钥 | `/etc/incubator/certs/server.pem` / `server.key` | `0644` / `0600` |

设备 Token 与看板密码**由安装脚本现场随机生成、只打印一次**，从未写入仓库、从未进入任何对话记录。

---

## 3. 总体架构

```
ESP32-S3 ──HTTPS POST──▶ Nginx :443 (175.24.182.190)
                            ├─ /api/v1/telemetry   → 反代；无 Basic Auth；Bearer + 限流 + 4k body
                            └─ / 及其它路径        → 反代 + Basic Auth（看板与查询接口）
                                   ↓
                            Uvicorn / FastAPI  127.0.0.1:8000   ← 绝不监听公网
                                   ↓
                            SQLite /var/lib/incubator/data.db（WAL）
```

设计原则：**不引入额外中间件**，复用服务器上已有的 Nginx；后端只依赖 `fastapi` + `uvicorn` 两个包；看板**零外部依赖**（不引 CDN、不引图表库，手写 canvas 画曲线）。

---

## 4. 服务器端方案

### 4.1 源码与落点

仓库 `server/` 目录（8 个文件）：

| 文件 | 作用 |
|---|---|
| `app.py` | 全部服务端逻辑：6 个接口、Pydantic 校验、常量时间鉴权、告警状态机、历史降采样 |
| `schema.sql` | `devices` / `telemetry` / `alerts` 三表 + 4 个索引（全部语句幂等，可重复执行） |
| `static/index.html` | 只读看板：状态条、四张指标卡、执行器状态、双 canvas 曲线（1h/6h/24h/7d）、告警表 |
| `tests/test_api_smoke.py` | 契约冒烟测试：**45 项断言**，只用标准库 + 真实 uvicorn 子进程 |
| `deploy/setup.sh` | 幂等安装脚本，**两阶段**（默认只装 API；`--with-nginx` 才动公网端口） |
| `deploy/incubator-api.service` | systemd 单元，只绑 `127.0.0.1:8000` + 内存/文件系统加固 |
| `deploy/incubator-https.conf` | Nginx 443 前端的**独立新文件**（不修改任何既有配置） |
| `README.md` | 运维说明（部署 / 回滚 / 排障 / CO₂ 单位警告） |

服务器上的落点：

```
/opt/incubator/app/            venv + app.py + schema.sql + static/ + tests/ + README.md
/var/lib/incubator/data.db     SQLite（WAL）
/etc/incubator/                devices.json + certs/
/etc/nginx/incubator.htpasswd
/etc/nginx/conf.d/incubator-https.conf
/etc/systemd/system/incubator-api.service
```

### 4.2 接口契约

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| POST | `/api/v1/telemetry` | 设备 Bearer | 上报。成功 **204 空体**；`401` token 错；`403` token 与 `device_id` 不匹配；`400` 校验失败 |
| GET | `/api/v1/devices` | Basic Auth | 设备列表 + 在线判定 + 样本数 |
| GET | `/api/v1/devices/{id}/latest` | Basic Auth | 最新一条 + 派生字段（`age_s`/`online`/`heater_pct`/`open_alerts`） |
| GET | `/api/v1/devices/{id}/history?hours=&step=` | Basic Auth | 曲线数据；`hours > 6` 时自动按 60 s 分桶；`hours ≤ 0` 或 `step < 5` 返回 400 |
| GET | `/api/v1/devices/{id}/alerts?hours=` | Basic Auth | 告警历史（最多 500 条） |
| GET | `/healthz` | Basic Auth（Nginx 后） | 健康检查；在 `127.0.0.1` 上直连不需要密码 |

`/docs`、`/redoc`、`/openapi.json` **已主动关闭**：裸 IP 无域名遮蔽，少一个暴露面。

### 4.3 上行载荷（权威契约）

`Content-Type: application/json` + `Authorization: Bearer <token>`。

```json
{
  "device_id": "incubator-001",
  "uptime_s": 183420,
  "firmware_version": "0.1.0",

  "temperature": 37.02, "temperature_ok": true,
  "humidity": 55.3,     "humidity_ok": true,
  "co2": 51000,         "co2_ok": true, "co2_available": true,

  "target_temperature": 37.0,
  "heater_on": true, "heater_pwm": 128,
  "light": false, "light_pct": 0,
  "fan": true,
  "valve": false,

  "rssi": -57,
  "heap_free": 210000, "heap_min": 180000, "heap_int": 120000
}
```

字段约束（服务端会拒绝越界值）：

| 字段 | 范围 | 为什么必须有 |
|---|---|---|
| `device_id` | 长度 1–64 | 必须与 Token 归属一致 |
| `temperature` | −40 … 85 | — |
| `humidity` | 0 … 100 | — |
| `co2` | 0 … 1000000 | **单位见 §4.4** |
| `target_temperature` | 0 … 120 | — |
| `heater_pwm` | 0 … 255 | 实际写入 LEDC 的占空 |
| `light_pct` | 0 … 100 | — |
| `rssi` | −120 … 0 | 诊断无线链路 |
| `heap_free` / `heap_min` / `heap_int` | 0 … 8388608 | 远端排查内存泄漏 |
| `temperature_ok` / `humidity_ok` / `co2_ok` | 布尔 | **缺了它看板无法区分"传感器故障"与"读数恰为 0"** |
| `co2_available` | 布尔 | 与 `co2_ok` 职责不同："传感器在不在线" vs "本次读数可不可信" |
| `heater_on` | 布尔 | **开关 ON + 传感器故障时 `heater_pwm=0`，与"用户主动关加热"的 0 含义完全不同** |
| `uptime_s` | ≥ 0 | 设备只报运行时长；**时间戳由服务器打**（`recv_ts`），故设备**不需要 NTP** |

**未知字段被忽略（不报错）**：固件先加字段、服务器后跟上时不会丢掉整条样本。代价是字段名写错会变成 NULL（看板可见），而不是整条丢失（不可见）。

### 4.4 ⚠️ CO₂ 单位尚未确认（服务器侧已隔离）

固件侧的实际代码是：

```cpp
const uint32_t scaledPpm = value * Config::Co2ScaleFactor;   // value = S8 Modbus 寄存器原值，因子 = 10
state.setCo2(scaledPpm, true, true);
Serial.printf("[CO2] Reading OK: %lu ppm ...", scaledPpm);   // 打印成 ppm，但已经 ×10
```

而 TFT 又用 `"ppm"` 作为单位标签显示该值。因此 `SystemState.co2` **不是裸 ppm**，其真实物理单位取决于 S8 的量程配置 —— **只有硬件能确认**。

**已裁决的处理方式**：上报值就是 `SystemState.co2`（×10 之后那个数），**服务器与看板都不换算**，于是看板显示与本机屏幕完全一致。由此产生两条硬约束：

1. 固件的阀门阈值（25000 / 40000 / 48000）与上报值**同单位**。在确认量程之前**不得**把它们当百分比解读，也不得用于任何跨系统换算。
2. 服务器侧**不实现任何 CO₂ 阈值告警**，直到单位被确认。届时改动点只有 `app.py` 的 `record_alerts` 一处。

### 4.5 数据库

```sql
devices   (device_id PK, label, first_seen, last_seen, fw_version)
telemetry (id PK, device_id, recv_ts, uptime_s, fw_version,
           temperature, temperature_ok, humidity, humidity_ok,
           co2, co2_ok, co2_available,
           target_temperature, heater_on, heater_pwm,
           light, light_pct, fan, valve,
           rssi, heap_free, heap_min, heap_int)
alerts    (id PK, device_id, raised_ts, cleared_ts, kind, detail)
          kind ∈ {offline, sensor_fault, low_heap}
```

- `journal_mode = WAL`；索引：`(device_id, recv_ts DESC)`、`(device_id, raised_ts DESC)`、`(device_id, kind) WHERE cleared_ts IS NULL`。
- **`recv_ts` 是服务器接收时刻**（权威时间），不是设备时间。
- 设备 Token **不入库**：数据库是进备份、进查询的那个东西，不该存凭据。
- 数据量（10 s 一条、单设备）：1 天约 8,640 行 / ~1 MB；1 月约 26 万行 / ~30 MB；1 年约 315 万行 / ~350 MB。
  ⚠️ 服务器只剩 8.0 G，**留存清理尚未实现**（属 P3）；短期无压力，长期必须做。

### 4.6 告警判定

- **离线**：设备重新上报时，若与上一条的时间差 > 3 × 上报间隔（30 s），写一条 `offline` 告警（`raised_ts` = 上一条时刻，`cleared_ts` = 恢复时刻）。
  **在服务器侧判定**，因此**不需要有人打开看板**也能记录断网。
- **传感器故障**：`*_ok` 由 true→false 时开一条 `sensor_fault`，恢复时补 `cleared_ts`。
- **内存偏低**：优先看 `heap_int`（仅内部 RAM），低于 40960 B 时告警。
- 告警只记录**状态跳变**，不随样本数膨胀。

### 4.7 安全措施（裸 IP 场景下要求更高）

- [x] 全站 HTTPS；证书为自签 CA 签发的 **IP 叶子证书**（`SAN=IP:175.24.182.190`）
- [x] 设备鉴权：`Authorization: Bearer`，服务端 `hmac.compare_digest` 常量时间比较，且**遍历全部设备不提前退出**（响应时延不泄露哪个 token 命中）
- [x] 看板 Basic Auth **强制**
- [x] 上报端点限流 `20r/m burst=10`、`client_max_body_size 4k`、`limit_except POST { deny all; }`
- [x] Pydantic 范围校验 + 字段限长
- [x] FastAPI 只绑 `127.0.0.1`；`/docs` 全关
- [x] `devices.json`、`certs/*.key`、`secrets.ini`、固件 CA 头文件 **全部 gitignore**

### 4.8 部署（两步，可回滚）

```bash
# 第一步：只装 API 到 loopback，不碰任何公网端口
sudo bash <bundle>/server/deploy/setup.sh

# 第二步：自签 IP 证书 + 443 前端 + Basic Auth
sudo bash <bundle>/server/deploy/setup.sh --with-nginx
```

脚本**幂等**：Token、看板密码、证书、数据库都**不会**被覆盖，代码文件每次都会更新。
第二步在 `nginx -t` 失败时会**自动删除自己刚装的配置文件再退出**，因此不会把既有配置改坏。

**回滚**：

```bash
sudo rm /etc/nginx/conf.d/incubator-https.conf && sudo nginx -t && sudo systemctl reload nginx
sudo systemctl disable --now incubator-api
```

### 4.9 运维命令

```bash
journalctl -u incubator-api -f                 # 实时日志
systemctl restart incubator-api                # 重启
sudo nano /etc/incubator/devices.json          # 改设备显示名（按 mtime 自动重载，无需重启）
sudo -u incubator sqlite3 /var/lib/incubator/data.db \
  "SELECT datetime(recv_ts,'unixepoch','+8 hours') t, temperature, heater_pwm
   FROM telemetry ORDER BY recv_ts DESC LIMIT 10;"
sudo -u incubator /opt/incubator/app/venv/bin/python /opt/incubator/app/tests/test_api_smoke.py

# 备份（数据库是唯一有状态的资产）
sudo systemctl stop incubator-api
sudo cp /var/lib/incubator/data.db /root/incubator-$(date +%F).db
sudo systemctl start incubator-api

# 重置看板密码
P=$(openssl rand -base64 24 | tr -dc 'A-Za-z0-9' | cut -c1-20)
printf '%s' "$P" | openssl passwd -apr1 -stdin > /tmp/h
sudo cp /tmp/h /etc/nginx/incubator.htpasswd && rm /tmp/h
sudo chown root:www-data /etc/nginx/incubator.htpasswd && sudo chmod 0640 /etc/nginx/incubator.htpasswd
echo "$P"
```

---

## 5. 服务器端进度

### 5.1 已完成并验证

| 项 | 验证方式 | 结果 |
|---|---|---|
| 代码编写 | 开发机 + 服务器 | 8 个文件 |
| 契约测试 | 真实 uvicorn 子进程 + 标准库 HTTP | **45/45 PASS**（开发机与服务器各跑一次；服务器上以 `incubator` 用户身份运行，同时证明服务账号可读 app 目录与 venv） |
| 看板前端 | Node 语法检查 + DOM 一致性静态检查 | JS 语法 OK；11/11（无重复 id、JS 引用的每个 id 都存在、无外部资源、无内联事件） |
| 安装脚本 | `bash -n` | 通过 |
| 行尾/编码 | 逐字节扫描 + `git ls-files --eol` | 全部 LF、无 BOM、`i/lf w/lf` |
| 部署 | 两次 `setup.sh`（含 `--with-nginx`） | 见 §5.2 |
| 固件侧未受影响 | 部署后 `pio run` | SUCCESS，数字与部署前**一字不差** |

### 5.2 外部独立验收（从开发机打向公网 IP，不依赖服务器输出）

| # | 检查 | 结果 |
|---|---|---|
| 1 | TCP 443 | 部署前 `ConnectionRefused` → 部署后 **`OPEN`** ✓ |
| 2 | TCP 8000 | `TimedOut` → **API 未泄露到公网** ✓ |
| 3 | TLS 证书 | `subject=CN=incubator-server`、`issuer=CN=Incubator Local CA`、**`SAN=IP Address:175.24.182.190`**、有效期至 **2028-12-21**（825 天）✓ |
| 4 | `GET /` 匿名 | **401** + `WWW-Authenticate: Basic realm="Incubator"` ✓ |
| 5 | `GET /` 用空密码 | **401** → 证明服务器存的**不是**空密码的哈希 ✓ |
| 6 | `POST /api/v1/telemetry` 无 token | **401** ✓ |
| 7 | `POST` 用伪造的 64 位 Bearer | **401** ✓ |
| 8 | 既有 OTA 服务 | `http://IP/` 200、`/fw/` **403**、`/fw/不存在.bin` **404** —— 与改动前基线**逐条一致** ✓ |
| 9 | 限流 | 25 发快速 POST → **9×401 + 16×503** ✓ |
| 10 | 浏览器人工验收 | 输入密码后看板正常打开（2026-09-18）✓ |

### 5.3 未完成

- ⬜ **看板上的曲线**：要等真实设备上报；现在是空管道
- ⬜ **手机 4G（关闭 Wi-Fi）访问的实测复述**：设计上成立（IP 不受备案拦截），但尚未有人明确复述测过
- ⬜ `retention.py` 留存清理（90 天 + 小时聚合）—— P3
- ⬜ CO₂ 阈值告警 —— 等单位确认（§4.4）

---

## 6. 单片机端方案

### 6.1 现状

固件（ESP32-S3，Adafruit Feather）**温控本体已完成**，`src/` 下 11 个模块 + `main.cpp` + `Config.h`，全部状态集中在 `struct SystemState`。联网部分**一行代码都没有**。

联网模块**只需读取 `SystemState`**，不修改 PID / 传感器 / 阀门 / 风扇 / 显示逻辑。

### 6.2 计划新增

```
src/NetworkConfig.h        非敏感联网参数（上报周期、超时、任务栈/核/优先级、堆阈值）
src/WiFiManager.h/.cpp     WiFi STA 状态机 + 指数退避重连（非阻塞）
src/TelemetryClient.h/.cpp HTTPS 上报 + Bearer 鉴权 + FastAPI 快照
（不需要 TimeSync / NTP —— 时间戳由服务器打）
secrets.ini                gitignore：SSID / 密码 / ServerUrl / DeviceToken
secrets.example.ini        入库的模板
src/ServerCaCert.h         gitignore：内嵌的 CA 公钥证书（多行 PEM，不适合走 -D）
src/ServerCaCert.h.example 入库的模板
```

`WiFi` / `HTTPClient` / `WiFiClientSecure` 均为 arduino-esp32 内置，**不新增库依赖**。
`main.cpp` 的 `loop()` 末尾追加两个调用（**不插入中间**，避免影响现有模块相对时序）：

```cpp
wifiManager.update(now);
telemetryClient.update(now, systemState);
```

### 6.3 ⚠️ 并发模型（必须先裁决）

**问题**：`HTTPClient` / `WiFiClientSecure` 是**同步阻塞 API**。Wi-Fi 连接可以靠轮询 `WiFi.status()` 做到非阻塞，**HTTP 请求本身不行**。

**实测到的超时真相**（读 arduino-esp32 3.0.7 源码得到，不是估计）：

| 阶段 | 受谁控制 | 默认值 | 显式设限后 |
|---|---|---|---|
| TCP connect | `HTTPClient::setConnectTimeout` | 5 s | 3 s |
| **TLS 握手** | **`WiFiClientSecure::setHandshakeTimeout`** | **120 s** | 8 s |
| 等响应 | `HTTPClient::setTimeout` | 30 s | 3 s |
| **合计最坏阻塞** | | **~125 s** | **~14 s** |

**关键陷阱**：`HTTPClient::setTimeout()` **管不到 TLS 握手** —— 它只调 `_client->setTimeout()`，而握手超时是 `WiFiClientSecure` 的独立字段，**只能**用 `setHandshakeTimeout()` 压。漏掉这一句，一次普通的"服务器不可达/反代挂了/证书不匹配"就能让主循环**静默卡 2 分钟**。

**而且是静默的**：`loopTask` 默认**不订阅**任务看门狗（订阅要走显式的 `enableLoopTaskWatcher()`，本仓库没有调用），所以卡住**不会重启、不会报错**，只会安静降级。

**为什么这关乎安全**：现有代码里两处对主循环延迟敏感 ——

| 受影响模块 | 机制 | 后果 |
|---|---|---|
| 阀门 `pulseValve()` | 关阀靠 `millis() - lastOpenTime >= pulseTime`，**没有硬件定时器**，只有主循环再次轮询时才关 | 卡 T 毫秒 ⇒ **阀门实际开启时长 = 标称 + T**，线性无补偿。标称值只有 **500 ms**（短脉冲）/ 1000 ms（长脉冲），正常一次 HTTPS 往返的 100–200 ms 抖动就已经是 **+20~30% 的 CO₂ 过量注入** |
| 加热 PID | `Compute()` 按 1 s 自节流，积分增量绑定 SampleTime 而非真实时差 | 卡 ≥2 s 丢一次积分；卡 14 s 丢 13 次 |

**两个方案**：

**方案 1（推荐）：Core 0 FreeRTOS 任务 + 主循环接口不变**

已核实的平台事实：Wi-Fi 协议栈任务钉在 **Core 0**；Arduino `loopTask` 在 **Core 1**；tick 1000 Hz。

```
Core 1（loopTask，控制环，永不让出）        Core 0（netTask，新增）
  SHT30 采样 / S8 轮询                        vTaskDelay(20ms) 让出
  PID Compute / ledcWrite / 阀门 / 风扇        WiFi.status() 判定
  TFT 显示 / 旋钮输入                          到点 → 取快照 → HTTPS POST（阻塞只在这里）
  telemetryClient.update() ──portMUX 临界区取快照──▶
        ◀── volatile 标志回读（wifiOk/cloudOk/httpCode/rssi）
```

两条不变量让它干净落地：

1. **`SystemState` 的写入永远只发生在 Core 1**，Core 0 从不触碰 → 显示与控制路径**完全不需要加锁**，耦合不扩散。
2. **Core 0 → Core 1 的回传只用 `volatile` 标量**（bool / int8 / int16 的对齐单次写），微秒级回读。

`update()` 在 Core 1 上只做「临界区内 14 个 POD 字段赋值 + 回读 volatile」，**微秒级返回**。

代价：+12 KB 内部 RAM 任务栈；约 60 行 RTOS 样板；`netTask` 必须规律 `vTaskDelay` 让出，否则 Core 0 的 idle 任务饿死超过 5 s 会触发**看门狗 PANIC 重启**（比卡一下更糟）。

**方案 2（不推荐）：主循环同步 POST**

代码最简（无 RTOS、无锁），但**在健康网络下也不合格**：正常一次上报往返 100–200 ms，每 10 s 注入一次抖动，阀门误差 +20~30%；遇到故障则卡到 14 s（漏设握手超时则 125 s），阀门开 14.5 s。且失败模式**无看门狗捕获、静默发生**。

它的"低成本"是幻觉：要真正非阻塞，就得在 `WiFiClientSecure` 之上自己实现分步状态机与非阻塞 HTTP，**比方案 1 难写 3~5 倍**。

**结论**：选**方案 1**。判据是 §1.3 的硬约束与"拔掉路由器后阀门脉冲时长必须无可测差异"这一验收标准 —— 方案 2 物理上过不了。

### 6.4 配置注入

```ini
; secrets.ini —— 绝不提交
[secrets]
build_flags =
    -DSECRET_WIFI_SSID=\"...\"
    -DSECRET_WIFI_PASS=\"...\"
    -DSECRET_SERVER_URL=\"https://175.24.182.190/api/v1/telemetry\"
    -DSECRET_DEVICE_ID=\"incubator-001\"
    -DSECRET_DEVICE_TOKEN=\"<从服务器 /etc/incubator/devices.json 取>\"
```

`platformio.ini` 通过 `extra_configs = secrets.ini` 引入，且**在 `secrets.ini` 缺失时要有兜底**（否则别人克隆后无法编译）。

CA 证书是多行 PEM，不适合走 `-D` 注入 → 单独放 `src/ServerCaCert.h`（gitignore），仓库留 `.example`。
固件只需**CA 公钥证书**，服务器上的 `incubator-ca.key` **绝不进固件**。

### 6.5 字段映射

```
temperature      → temperature        temperatureValid → temperature_ok
humidity         → humidity           humidityValid    → humidity_ok
co2              → co2（原值，不换算）  co2Valid → co2_ok，co2Available → co2_available
setpoint         → target_temperature
heatingEnabled   → heater_on          heaterDuty       → heater_pwm
lightEnabled     → light              lightBrightness  → light_pct
fanEnabled       → fan                valveOpen        → valve
millis()/1000    → uptime_s           ESP.getFreeHeap()      → heap_free
ESP.getMinFreeHeap() → heap_min       heap_caps_get_free_size(INTERNAL|8BIT) → heap_int
WiFi.RSSI()      → rssi               NetworkConfig::FirmwareVersion → firmware_version
```

**为什么 `heap_int` 要紧**：本板有 2 MB PSRAM，且 `CONFIG_SPIRAM_USE_MALLOC 1`、`SPIRAM_MALLOC_ALWAYSINTERNAL 4096`（≤4 KB 强制内部 RAM，更大的优先落 PSRAM）。因此 `ESP.getFreeHeap()` **会把 PSRAM 算进去**，单看它会漏掉内部堆泄漏 —— 而内部堆才决定"还能不能再开一个 TLS 会话"。`heap_min`（自启动以来最低水位）比瞬时值灵敏得多。

### 6.6 失败模式与取舍

| 故障 | 设备侧行为 | 用户可见 |
|---|---|---|
| WiFi 断开 | 指数退避重连（5 s → 60 s 封顶），**控制环完全不受影响** | 看板 OFFLINE |
| 服务器不可达 / TLS 失败 | 记录错误码，**下一周期**再试，不加速重试 | 看板 OFFLINE |
| Token 错误 401 | 打印一次告警并降频重试 | 看板无数据 |
| 堆不足（< 40 KB） | 跳过本轮上报并打印，不冒险触发 OOM 重启 | 偶发缺口 |
| 服务器 5xx / 限流 503 | 丢弃本周期数据 | 短暂缺口 |

**明确取舍**：

- **不做本地缓存补传**：断网期间数据直接丢弃。"知道断过网"比"补齐历史"更重要，且避免频繁写 flash。
- **不做 QoS 保证**：HTTP 尽力而为，符合只读监控定位。
- **网络故障永不升级为控制故障**：任何网络异常都不触碰 `heatingEnabled` / `setpoint` / 阀门等控制状态。

### 6.7 资源预算

| 项目 | 当前（含温控本体） | 预估新增 | 评估 |
|---|---|---|---|
| Flash | 477,781 B / 1,441,792 B（**33.1%**） | WiFi + TLS + HTTP 约 **+300~500 KB** | 预计 ~800–980 KB，**可容纳但必须实测** |
| RAM（静态） | 39,548 B / 327,680 B（**12.1%**） | netTask 栈 12 KB（**必须内部 RAM**）+ TLS 缓冲（可能落 PSRAM） | 余量充足 |

**Flash 是真正的瓶颈**：应用分区只有 **1.41 MB**。若加入 TLS 后超限，可选项是裁剪 mbedTLS cipher suite、`-Os`，或**放弃 OTA 双分区**（这是需要专门决策的取舍，不是随手可做的优化）。

⚠️ netTask 的栈有被分配到 PSRAM 的风险（`SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY 1` 且 12 KB > 4096）。编码时需**显式验证栈落位**（`heap_caps_get_free_size(INTERNAL)` 前后差），必要时改用 `xTaskCreateStatic` + 显式内部 RAM 分配。

---

## 7. 单片机端进度

### 7.1 已完成（与本功能无关，但它是数据源）

- 温控闭环：双增益 PID（保守 Kp=10 / Ki=0.1；激进 Kp=20；3 ℃ 切换带），采样 1 s
- 传感器：SHT30（温湿度，I²C 0x44）、SenseAir S8（CO₂，UART1 Modbus，非阻塞轮询）
- 执行器：加热 PWM（8 位，1 kHz）、风扇、光照 PWM、CO₂ 阀门（脉冲式）
- 交互：旋转编码器 + 按键 + 320×240 TFT
- 设置持久化（手动保存，不频繁写 NVS）
- 加热开关与传感器故障**解耦**：故障时只暂停输出，**开关保持 ON**
- `pio run` SUCCESS

### 7.2 未开始

- ⬜ 全部联网模块（§6.2）
- ⬜ Token 与 CA 的注入、`platformio.ini` 的 `extra_configs` 兜底
- ⬜ 实机烧录验证（含"拔掉路由器后阀门脉冲时长与联网时无可测差异"这一验收项）
- ⬜ 连续运行 24 h 观察 `heap_int` 是否单调下降（泄漏检测）

### 7.3 与本功能相邻的遗留问题

| # | 问题 | 影响 |
|---|---|---|
| 1 | **CO₂ 单位未确认**（§4.4） | 上报值可用（与屏幕一致），但**不能**据此做阈值告警或换算 |
| 2 | **PID 参数未随 SHT30 复核** | 增益是围绕上一代传感器定的；换型后纯滞后变化会影响 Kp 上限，表现为过冲或振荡。培养箱对**过冲**比恒温精度更敏感 |
| 3 | 静默暂停耦合 | 传感器故障时输出为 0 而屏幕仍显示"加热 ON"；上机时需统计连续 I²C 读失败频率，若频繁应在驱动层加重试 |

---

## 8. 待办汇总

| 优先级 | 项 | 归属 |
|---|---|---|
| **阻塞** | 并发模型裁决（§6.3，推荐方案 1） | 决策 |
| 高 | 固件联网模块编码 + `secrets.ini` 注入 | 单片机端 |
| 高 | 实机烧录 + 隔离性验收（拔网 / kill 服务器 / 不存在地址三种情形） | 单片机端 |
| 中 | CO₂ 单位确认（据 S8 量程），据此决定是否加阈值告警 | 硬件 / 单片机端 |
| 中 | PID 参数随 SHT30 复核 | 单片机端 |
| 中 | 手机 4G 访问实测复述 | 验收 |
| 低 | `retention.py` 留存清理（90 天 + 小时聚合） | 服务器端 |
| 低 | 处理服务器日志占用（约 2.1 GB，可释放约 1.5 GB） | 运维 |
| 低 | 电脑端导入自签 CA 以消除浏览器警告（**手机不建议**：那等于把根 CA 装进手机） | 运维 |
| 可选 | ICP 备案 → 切域名 + 90 天证书 | 可选优化 |

---

## 9. 实测踩过的坑（避免重复）

**系统层**

| 坑 | 后果 | 正确做法 |
|---|---|---|
| nginx **1.24** 不认 `http2 on;`（1.25.1+ 才有） | `nginx -t` 直接失败 | 用 `listen 443 ssl;`，不启用 HTTP/2 |
| htpasswd 放在 `0750 root:incubator` 的目录里 | nginx worker（`www-data`）读不到 → **403 且原因极难定位** | htpasswd 放 `/etc/nginx/`，`0640 root:www-data` |
| 以为 `ssl_certificate_key` 必须放开权限 | 白送风险 | nginx **master 以 root 启动时读取**，`0600 root:root` 完全可用 |
| `HTTPClient::setTimeout()` 不覆盖 TLS 握手 | 默认握手超时 **120 s**，主循环静默卡 2 分钟 | 必须显式 `setHandshakeTimeout()` |
| SQLite 的 INTEGER 列返回 `0/1` 而非 `True/False` | `previous is False` 恒为假 → **故障恢复后告警永不清除** | 比较**真值**（`bool()`），不要比身份 |
| `ESP.getFreeHeap()` 把 PSRAM 计入 | 内部堆泄漏被掩盖 | 同时报 `heap_min` 与 `heap_int` |
| FastAPI 的 `jsonable_encoder()` **没有** `fallback` 参数 | 校验失败时 400 处理函数自己抛错 → 500 | 自定义安全序列化（`ctx` 里的对象转 str） |
| 载荷字段名与 API 输出字段名不一致 | 看板永久显示 `--` 且无任何报错 | 契约测试里断言字段名 |

**开发机工具链**

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 用 PowerShell `Get-Content` 数中文文件行数 | 同一文件真实 926 行，报 611 / 503 → 得出"文件被截断"的**假结论** | 用 Python 或结构化读取工具数 |
| 含中文的脚本经 PowerShell 管道喂给解释器 | 脚本源码被按 GBK 编成 `?`，**中文比较全变假、ASCII 检查仍为真**（半对半错最难发现） | 把脚本落盘再执行 |
| Windows PowerShell 5.1 的 `Compress-Archive` | zip 条目名带反斜杠，Linux 解压出一堆 `server\app.py` | 用 `tar -a -c -f`，然后**读回校验条目名** |
| `scp D:\path\file.zip user@host:/tmp/` | scp 把 `D:` 当主机名 → `Could not resolve hostname D` | 先 `cd` 到目录，再用相对文件名 |

---

## 10. 本文件变更记录

| 日期 | 变更 |
|---|---|
| 2026-09-18 | 首次建立。服务器端已完成部署与外部验收；单片机端联网未开工；并发模型待裁决 |