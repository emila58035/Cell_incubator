# 远程访问功能 · 开发方案与进度（独立参考文档）

> **独立性声明**：本文档独立维护，**不引用、不依赖**仓库内任何其它文档。
> 本文所写的一切均以**当前代码与实测结果**为准；事实变化时**只改本文**，不需要同步到别处。
>
> 覆盖范围：培养箱远程监控功能的两端 —— **服务器端**（已上线）与**单片机端**（已编码、未实机验证）。
> 最后更新：**2026-09-19**（联网模块落地并实测：§6.2 / §6.4 / §6.7 / §7.2 已更新，§9 增补固件联网坑表）。

---

## 0. 状态速览

| 部分 | 状态 | 证据 |
|---|---|---|
| 服务器端后端 | ✅ **已上线** | systemd `active`、只绑 `127.0.0.1:8000`、SQLite 三表 WAL |
| HTTPS 入口 | ✅ **已上线** | `https://175.24.182.190/` 可访问，自签 IP 证书，Basic Auth 生效 |
| 只读看板 | ✅ **已上线** | 浏览器输入密码可正常打开（已于 2026-09-18 人工验收） |
| 数据链路 | ⬜ **空管道** | 看板显示"尚无设备上报"，因为**没有任何设备在上报** |
| 单片机端（温控本体） | ✅ 已开发完成 | 未接入联网模块时：`pio run` SUCCESS，Flash 477,781 B / 1,441,792 B（33.1%），RAM 39,548 B（12.1%） |
| 单片机端（联网模块） | ✅ **已编码 + 已烧录** | 并发模型按方案 1 实现（§6.3）；`pio run` SUCCESS，Flash **1,123,477 B / 2,097,152 B（53.6%）**，RAM 58,612 B（17.9%）。2026-09-19 首次实机烧录并抓到完整启动日志（§7.2）；**尚未联调**（无真凭据） |
| Flash 分区表 | ✅ **已切 8 MB** | `tinyuf2-partitions-8MB.csv` + `board_upload.flash_size = 8MB`；`partitions.bin` 解码核对：`ota_0`@0x10000 / `ota_1`@0x210000 各 2048 KB，`nvs`/`otadata` 偏移未变（§6.8） |
| 实机（裸板） | ✅ 已接上 | ESP32-S3 v0.2、8 MB flash（eFuse quad）、**8 MB 八线 PSRAM**、MAC `3c:0f:02:dc:f1:84`；CH340K@COM22 与原生 USB 两路均可用（§6.8） |
| 端到端联调 | ⬜ 未开始 | 实机已就位，只缺真凭据（`secrets.ini`、`src/ServerCaCert.h`） |

**当前阻塞**：只剩**真凭据**（设备 Token 与服务器 CA 证书）。实机已接上并完成首次烧录。

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

固件（ESP32-S3R8，硬件事实见 §6.8）`src/` 下共 12 个模块（各 `.h`/`.cpp`）+ `main.cpp` + `Config.h` + `NetworkConfig.h` + `User_Setup.h`，全部状态集中在 `struct SystemState`。温控本体已完成；联网模块见 §6.2，2026-09-19 已编码，**未实机验证**。

联网模块**只需读取 `SystemState`**，不修改 PID / 传感器 / 阀门 / 风扇 / 显示逻辑。

### 6.2 实际新增（已编码）

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
`main.cpp` 的 `loop()` 末尾追加一个调用（**不插入中间**，避免影响现有模块相对时序）：

```cpp
telemetryClient.update(now, systemState);
```

⚠️ **与早期草案的偏差（以已实现为准）**：`WiFiManager::step()` 由 netTask 在 **Core 0** 上调用，**不在** `loop()` 里。草案曾把 `wifiManager.update(now)` 写在 `loop()` 中，那会让 Core 1 直接调 WiFi API（`WiFi.status()` 内部要取锁），与 §6.3 的“WiFi 判定只在 Core 0”自相矛盾。现在 Core 1 只做一件事：`update()` 里微秒级发布快照；链路状态经 `linkUp()` / `cloudOk()` / `rssi()` / `lastHttpCode()` 这几个 volatile 访问器回读。

### 6.3 ⚠️ 并发模型（已裁决：方案 1）

**问题**：`HTTPClient` / `WiFiClientSecure` 是**同步阻塞 API**。Wi-Fi 连接可以靠轮询 `WiFi.status()` 做到非阻塞，**HTTP 请求本身不行**。

**实测到的超时真相**（读 arduino-esp32 3.0.7 源码得到，不是估计）：

| 阶段 | 受谁控制 | 默认值 | 显式设限后 |
|---|---|---|---|
| TCP connect | `HTTPClient::setConnectTimeout` | 5 s | 3 s |
| **TLS 握手** | **`WiFiClientSecure::setHandshakeTimeout`** | **120 s** | 8 s |
| 等响应 | `HTTPClient::setTimeout` | 5 s | 3 s |
| **合计最坏阻塞** | | **~130 s** | **~14 s** |

**关键陷阱**:`HTTPClient::setTimeout()` **管不到 TLS 握手** -- 它只调 `_client->setTimeout()`,而握手超时是 `WiFiClientSecure` 的独立字段,**只能**用 `setHandshakeTimeout()` 压。漏掉这一句,一次普通的"服务器不可达/反代挂了/证书不匹配"就能让主循环**静默卡 2 分钟**。

**单位陷阱**：`setHandshakeTimeout()` 的参数是**秒**（源码 `WiFiClientSecure.cpp` 里 `handshake_timeout = 参数 * 1000`，两个构造路径的默认值都是 `120000` ms）。写成 `setHandshakeTimeout(8000)` 会变成 8000 秒，等于取消超时。

**修正（2026-09-19）**：上表里“等响应”的默认值实测是 **5 s**，不是 30 s（`HTTPClient.h:42` `#define HTTPCLIENT_DEFAULT_TCP_TIMEOUT (5000)`，连接与响应都用它）；因此最坏合计是 5 + 120 + 5 = **~130 s**。结论不变：120 s 的握手超时是主项。

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

代码最简（无 RTOS、无锁），但**在健康网络下也不合格**：正常一次上报往返 100–200 ms，每 10 s 注入一次抖动，阀门误差 +20~30%；遇到故障则卡到 14 s（漏设握手超时则 130 s），阀门开 14.5 s。且失败模式**无看门狗捕获、静默发生**。

它的"低成本"是幻觉：要真正非阻塞，就得在 `WiFiClientSecure` 之上自己实现分步状态机与非阻塞 HTTP，**比方案 1 难写 3~5 倍**。

**结论（2026-09-19 已裁决）**：选**方案 1**，且已按此实现。判据是 §1.3 的硬约束与"拔掉路由器后阀门脉冲时长必须无可测差异"这一验收标准 —— 方案 2 物理上过不了。

实现细节与草案的两处收紧：

1. **快照字段全部 ≤4 字节且 4 字节对齐**。`SystemState` 里的 `double setpoint` / `double heaterOutput` 是两次 32 位访问，跨核读会**撕裂**（撕裂的 `setpoint` 会被服务器 400 拒掉整条样本），所以发布时把 `setpoint` 窄化为 `float`，`heaterOutput` **不上报**（契约里没有对应字段）。8 个布尔量打包进一个 `uint8_t`。
2. **临界区两端共用一个 `portMUX_TYPE`**。只锁读端不构成互斥：`portENTER_CRITICAL` 的跨核互斥来自 spinlock，只有写端（Core 1 的 `publish()`）也拿同一把锁才成立。临界区只包住赋值，**绝不跨越任何阻塞调用**。

### 6.4 配置注入（2026-09-19 实测）

`secrets.ini`（gitignore，模板 `secrets.example.ini` 入库）：

```ini
[secrets]
build_flags =
	-DSECRET_WIFI_SSID=\"...\"
	-DSECRET_WIFI_PASS=\"...\"
	-DSECRET_SERVER_URL=\"https://<server-address>/api/v1/telemetry\"
	-DSECRET_DEVICE_ID=\"incubator-001\"
	-DSECRET_DEVICE_TOKEN=\"<从服务器 /etc/incubator/devices.json 取>\"
```

`platformio.ini` 侧五条约束，每一条都是实测踩出来的：

1. **`extra_configs` 必须写在 `[platformio]` 段。** PlatformIO 读的是 `self.get("platformio", "extra_configs", [])`；写在 `[env:...]` 段**不报错、被静默忽略**（`pio project config` 里能看到该选项，但值仍是主文件的兜底值）。
2. 用 `${PROJECT_DIR}/secrets.ini`，不要用裸文件名：`extra_configs` 走 `glob.glob()`，相对路径依赖进程 cwd。
3. 用 `${secrets.build_flags}` 插值把凭据拼进 env 的 `build_flags`。**不要**让 extra_configs 直接覆盖 env 的 `build_flags` —— INI 语义是整项替换，会连带丢掉 TFT 面板参数（屏仍能编译，但驱动是错的）。
4. 兜底：`platformio.ini` 自带一个 `[secrets]` 段（全空串）。`glob` 找不到文件时返回空列表而**不报错**，所以克隆后没有 `secrets.ini` 也能编译；空串在固件侧等于“未配置”，只打印一次并停止上报，不进入重试循环。
5. 验证方式：把标记值写进 `secrets.ini`，编译后直接在 `firmware.elf` 里搜该字符串（比看构建日志可靠）。

CA 证书是多行 PEM，不适合走 `-D` → 单独放 `src/ServerCaCert.h`（gitignore，模板 `.example` 入库），由 `NetworkConfig.h` 用 `__has_include` 引入；文件不存在时回落到空串，同样只在日志里说一次。
⚠️ 该头文件是在 `namespace NetworkConfig` **内部**被 include 的，所以**模板里不能再套一层命名空间**（否则报 `NetworkConfig::NetworkConfig::ServerCaCertPem`）。
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

**实现说明**：`setpoint`（`double`）在发布时**窄化为 `float`**；8 个布尔量打包进一个 `uint8_t flags`（见 §6.3）—— 两者都是为了让快照里每个字段都 ≤4 字节、跨核读不撕裂。

**为什么 `heap_int` 要紧**：芯片是 ESP32-S3R8，带 8 MB **八线** PSRAM，且 `CONFIG_SPIRAM_USE_MALLOC 1`、`SPIRAM_MALLOC_ALWAYSINTERNAL 4096`（≤4 KB 强制内部 RAM，更大的优先落 PSRAM）。PSRAM 一旦可用，`ESP.getFreeHeap()` **就会把外部内存算进去**，单看它会漏掉内部堆泄漏 —— 而内部堆才决定“还能不能再开一个 TLS 会话”。`heap_min`（自启动以来最低水位）比瞬时值灵敏得多。

⚠️ **当前构建下八线 PSRAM 实际未启用**（见 §6.8）：链接的是 quad 变体的 `libesp_psram.a`，所以这条“掩盖泄漏”的风险目前处于潜伏状态。三个 heap 字段**照报不变**：一旦切 `memory_type = qio_opi` 或换板，掩盖效应立刻生效，而遥测契约不应该随构建配置漂移。

### 6.6 失败模式与取舍

| 故障 | 设备侧行为 | 用户可见 |
|---|---|---|
| WiFi 断开 | 指数退避重连（5 s → 60 s 封顶），**控制环完全不受影响** | 看板 OFFLINE |
| 服务器不可达 / TLS 失败 | 记录错误码，**下一周期**再试，不加速重试 | 看板 OFFLINE |
| Token 错误 401 | 打印一次告警并降频重试 | 看板无数据 |
| 堆不足（< 40 KB 内部堆） | 跳过本轮上报并打印，不冒险触发 OOM 重启（一次 TLS 就要 ~32 KB） | 偶发缺口 |
| `secrets.ini` / `src/ServerCaCert.h` 缺失（回落到空串） | 打印一次“未配置”后**不再上报**，不进入重试循环 | 看板无数据 |
| SSID 为空（WiFi 未配置） | `begin()` 直接停用，`step()` 成为空操作 | 看板无数据 |
| 载荷超出固定缓冲区 | 丢弃本周期并打印，**不发送被截断的 JSON** | 偶发缺口 |
| 服务器 5xx / 限流 503 | 丢弃本周期数据 | 短暂缺口 |

**明确取舍**：

- **不做本地缓存补传**：断网期间数据直接丢弃。"知道断过网"比"补齐历史"更重要，且避免频繁写 flash。
- **不做 QoS 保证**：HTTP 尽力而为，符合只读监控定位。
- **网络故障永不升级为控制故障**：任何网络异常都不触碰 `heatingEnabled` / `setpoint` / 阀门等控制状态。

### 6.7 资源预算

| 项目 | 接入联网前 | 接入联网后（实测） | 评估 |
|---|---|---|---|
| Flash | 477,781 B（**33.1%**） | **1,123,477 B / 2,097,152 B（53.6%）**（8 MB 表，槽 2048 KB） | 相对联网前 +631 KB；关掉 USB CDC 又省回 36.8 KB；余量 **951 KB（46.4%）** |
| RAM（静态） | 39,548 B（**12.1%**） | 58,612 B / 327,680 B（**17.9%**） | 余量充足（含 12 KB 静态 netTask 栈；关掉 USB CDC 后比联网初版低 18.3 KB） |

**Flash：切了 8 MB 表，余量从 17% 变成 46%**。下面的数字都由 `pio run` 实测：

⚠️ **测量陷阱**：当前的 53.6% 是**空凭据**下的数字，会**低估真实体积**。凭据为空时编译器能把“未配置即返回”折叠掉，把请求路径的对象从静态库里排除掉 —— 同一份代码填上非空凭据后曾实测多出 **35 KB**（1,194,993 B 对 1,159,729 B）。真凭据比测试串更长，实际预期约 **56~57%**。

**已切到 8 MB 表**（2026-09-19，§6.8）：`board_build.partitions = tinyuf2-partitions-8MB.csv`，`ota_0`/`ota_1` 各 **2048 KB**，OTA 双分区保留。framework 另有 `tinyuf2-partitions-8MB-noota.csv`（单槽 **4096 KB**，放弃 OTA）与 `default_8MB.csv`（单槽 **3264 KB** + 64 KB `coredump`）—— 后者与**板上原有表一致**，是待决策项之一（§6.8）。

对比切表前后（同一份固件，非空凭据）：app 槽 1,408 KB → 2,048 KB，占用 82.9% → **57.0%**，余量 241 KB → **881 KB**。裁剪 cipher suite、`-Os` 这些是**真超限后**的备选，现在用不上了。

结论：**当前 8 MB 表下噪音已消失**。但记住两个约束：OTA 仍要求镜像小于**单个槽**（2048 KB），而 flash 占用仍远大于温控本体时期的 33% —— 后续加字段、加缓存时要继续盯 `pio run` 的分母。另外：当前表**没有 `coredump` 分区**，上电日志会打两遍 `No core dump partition found!`，崩溃拿不到回溯（§6.8 待决策 3）。

**栈落位已用静态分配彻底消除**：netTask 走 `xTaskCreateStaticPinnedToCore` + 静态 `StackType_t[3072]`，缓冲在 `.bss` → 内部 DRAM（预编译 libs 的 `SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` 未开），所以“栈会不会落 PSRAM”不再是运行时假设，而是编译期确定的。任务启动时还会打印 `esp_ptr_internal(taskStack_)` 供实机核对。这很重要：写 NVS 时 SPI flash 操作会禁用 cache，PSRAM 栈那一刻不可访问。

### 6.8 硬件事实与当前构建口径（2026-09-19 实测）

实物是 **ESP32-S3R8** 核心板，不是 Adafruit Feather。以下“当前构建口径”栏全部来自本机实测（board def JSON、`framework-arduinoespressif32-libs\esp32s3\sdkconfig`、`compile_commands.json`）：

| 项目 | 实物 | 当前构建口径 |
|---|---|---|
| Flash | BY25Q64 **8 MB** | **已切 8 MB 表** → app 上限 **2048 KB/槽**（2026-09-19）；`partitions.bin` 解码核对与 CSV 逐条一致 |
| PSRAM | **8 MB 八线（Octal）** | **实测不可用**（2026-09-19 首次上电）：启动即报 `quad_psram: PSRAM ID read error: 0x00ffffff ... wrong PSRAM line mode` + `psramAddToHeap(): PSRAM not initialized!`，`[BOOT]` 打印 `psram 0 B`。原因是链接了 `qio_qspi`（quad）变体的 `libesp_psram.a`（`compile_commands.json` 里 `qio_qspi` 72 次、`qio_opi` **0** 次）。修法是一行 `board_build.arduino.memory_type = qio_opi`（待决策） |
| 主控 | 240 MHz | 双核 SMP 成立（`# CONFIG_FREERTOS_UNICORE is not set`），tick 1000 Hz |
| USB | 双通道：CH340K（UART0）+ 原生 USB | `-DARDUINO_USB_CDC_ON_BOOT=0` + `build_unflags` 移除 board def 的 `=1` → `Serial` 走 **UART0**，日志与监视都用 **COM22**（实测抓到 `[BOOT]`/`[SETTINGS]`/`[NET]` 全部）；上传走 CH340K 自动下载电路 + `--upload-port COM22`，并用 `board_upload.use_1200bps_touch/wait_for_upload_port = false` 关掉 board def 的 1200bps-touch 流程。关掉 USB CDC 顺带省了 Flash 36.8 KB / RAM 18.3 KB |
| 显示 | ST7789 240×320，MOSI=21 SCLK=40 CS=16 DC=17 BL=15 RST=-1 | 配置收进 `src/User_Setup.h`，由 `build_flags`（`-DUSER_SETUP_LOADED=1` + `-include ${PROJECT_DIR}/src/User_Setup.h`）强制加载；实测启动打印 `[DISPLAY] Size=320 x 240, sprite=READY`（精灵图分配成功，面板实际点亮未验证） |
| 已用 IO | 28 个可编程 IO 中用了 18 个 | 全部避开八线 PSRAM 占用的 GPIO 33–37（这一点与 R8 相容） |

**看门狗事实（决定 §6.3 方案 1 的形态）**：`CONFIG_ESP_TASK_WDT_PANIC=y`、超时 5 s、**只勾选 `CHECK_IDLE_TASK_CPU0`**（`..._CPU1` 未勾选）。所以 Core 0 上的 netTask 不让出会直接 PANIC 重启；而 Core 1 的主循环永不让出是安全的 —— 这正是现状能跑的原因。

**已切**：8 MB 分区表（2026-09-19，见上表）。

**待决策**（都不是本功能的阻塞项）：

1. **`board_build.arduino.memory_type = qio_opi`**（启用八线 PSRAM）—— 现在有实测证据说它确实没启用（见上表）。代价一行配置 + 重编。注意：一旦启用，`ESP.getFreeHeap()` 会把 8 MB 外部内存算进去，上报的 `heap_free` 会突然变大（`heap_int`/`heap_min` 不受影响，仍是判断泄漏的口径）。本项目自身**不需要** PSRAM。
2. **`board` 定义是否脱离 Adafruit Feather** —— 现在仍用 Adafruit 的 `variant`（`pins_arduino.h`）与 `hwids`，`maximum_size` 还是 4 MB。风险低：引脚全在 `Config.h`/`User_Setup.h` 里显式写死，不依赖 variant 默认值。
3. **分区表要不要换成 `default_8MB.csv`** —— **实测：板上原本就是这张**。槽 **3264 KB**（现 2048 KB），而且**自带 64 KB `coredump` 分区**（当前报 `No core dump partition found!`，崩溃拿不到回溯）。

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

### 7.2 已编码 / 已烧录 / 未联调

**2026-09-19：首次实机烧录成功**（裸板，USB 供电，无任何外接器件）。实测启动日志（COM22）：

```
[BOOT] ESP32-S3 rev2, 2 core(s), flash 8388608 B, psram 0 B, heap 312116 B
[HEATER] Ready on GPIO39; startup state=OFF
[SETTINGS] No valid saved settings; using defaults
[SETTINGS] Active: setpoint=37.0 C, light=OFF/0%, fan=OFF
[DISPLAY] Size=320 x 240, sprite=READY
[SHT30] ERROR: sensor not detected at 0x44
[HEATER] Sensor fault: heating paused, switch stays ON
[WIFI] No SSID configured (secrets.ini missing?); networking stays off
[NET] netTask on core 0, stack 12288 B at 0x3fc9b090 (internal=1)
[CO2] ERROR: connection response timeout, elapsed=500 ms
```

已由硬件证实：

- ✅ **`[NET] netTask on core 0` + `internal=1`** —— 静态栈确实落在内部 DRAM（这是 §6.3 最关键的安全不变量）
- ✅ **`flash 8388608 B`** —— `board_upload.flash_size = 8MB` 在镜像头生效
- ✅ **`psram 0 B`** —— 八线 PSRAM 确实没启用（§6.8）
- ✅ **30 s 监听：0 次重启、0 次 panic/看门狗** —— netTask 的 `vTaskDelay` 让出纪律站得住，主循环的快照发布/回读路径不死锁
- ✅ 加热默认 ON 但在无 SHT30 时**自动暂停**（`Sensor fault: heating paused`）

仍待完成：

- ⬜ **实机隔离性验收**：拔掉路由器 / kill 服务器 / 指向不存在地址三种情形下，阀门脉冲时长与联网时无可测差异
- ⬜ 连续运行 24 h 观察 `heap_int` 是否单调下降（泄漏检测）
- ⬜ **真凭据 + 端到端联调**（`secrets.ini` + `src/ServerCaCert.h`）—— 当前唯一阻塞
- ✅ 凭据注入两条路径已实测（标记串进 ELF、删文件后仍能编译且标记消失）
- ✅ 栈落位：静态分配，`.bss` → 内部 DRAM（并已在硬件上打印验证）

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
| ~~阻塞~~ | ~~并发模型裁决（§6.3）~~ | ✅ 2026-09-19 已裁决为方案 1 |
| ~~高~~ | ~~固件联网模块编码 + 凭据注入~~ | ✅ 2026-09-19 已编码，`pio run` SUCCESS |
| 高 | ~~实机烧录~~ + 隔离性验收（拔网 / kill 服务器 / 不存在地址三种情形） | ✅ 烧录已完成（§7.2）；验收待做 |
| 高 | 填真凭据并端到端联调（`secrets.ini` + `src/ServerCaCert.h`）—— **当前唯一阻塞** | 单片机端 |
| ~~中~~ | ~~决定是否切 8 MB 分区表~~ | ✅ 2026-09-19 已切（§6.7：余量 17% → 43%） |
| 中 | 决定是否切 `memory_type = qio_opi` 启用八线 PSRAM（已实测确认当前不可用） | 决策 |
| 中 | 决定分区表是否换成 `default_8MB.csv`（槽 3264 KB + 自带 coredump；与板上原表一致） | 决策 |
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

**固件联网（2026-09-19 实测）**

| 坑 | 后果 | 正确做法 |
|---|---|---|
| `extra_configs` 写在 `[env:...]` 段 | PlatformIO **静默忽略**（只读 `[platformio]` 段）→ `secrets.ini` 形同不存在，固件用空凭据编译 | 放进 `[platformio]` 段；用 `pio project config` 核对值是否真被替换 |
| `extra_configs` 用裸文件名 | 走 `glob.glob()`，相对路径依赖进程 cwd | 用 `${PROJECT_DIR}/secrets.ini` |
| 让 extra_configs 直接覆盖 env 的 `build_flags` | INI 语义是整项替换，会连带丢掉 TFT 面板参数（屏还能编译，但驱动错了） | 用 `${secrets.build_flags}` 插值拼进原有 `build_flags` |
| 在**空凭据**下测固件体积 | 编译器把“未配置即返回”折叠掉，请求路径对象不进镜像 → **低估约 35 KB** | 评估预算时用非空凭据测（§6.7） |
| 被 `namespace` 内部 include 的头文件里再写 `namespace` | `NetworkConfig::NetworkConfig::ServerCaCertPem` 编译失败 | 模板头文件里**不要**套命名空间 |
| `heap_caps_get_free_size()` / `esp_ptr_internal()` 直接调用 | 报未声明：`Arduino.h` 不带这两个 | 显式 include `esp_heap_caps.h` / `esp_memory_utils.h` |
| board def 的 `use_1200bps_touch` + `wait_for_upload_port` | 对 CH340 那路做 1200bps 触摸 → 板子复位、原生 USB 换号 → PlatformIO 切到新口后报 `Cannot configure port ... PermissionError(13)`（看起来像驱动/句柄问题，其实是它自己换了口） | 有 CH340 自动下载电路的板子：`board_upload.use_1200bps_touch = false` + `wait_for_upload_port = false`，并用 `--upload-port` 指定 |
| 以为 USB CDC 会像 UART 一样保留输出 | CDC 在主机未打开时**直接丢弃**输出，且每次复位换 COM 号 → 启动日志永远抓不到（本机实测 COM29 → COM30） | 带 CH340 的板子把 `Serial` 留在 UART0：`-DARDUINO_USB_CDC_ON_BOOT=0` + `build_unflags = -DARDUINO_USB_CDC_ON_BOOT=1`（只用 `-D` 会与 board def 的 `=1` 打架，靠命令行顺序决胜） |
| 分区表没有 `coredump` 分区 | 上电打印两次 `esp_core_dump_flash: No core dump partition found!`，崩溃拿不到回溯 | 选带 coredump 的表（`default_8MB.csv` 有 64 KB） |
| 以为换分区表必须整片擦除 | —— | 实测 **不需要**：PlatformIO 上传时会写 `boot_app0.bin` 到 `0xE000` 覆盖 otadata（写入日志与回读均确认） |

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
| 2026-09-19 | 按本机实测修正三处：硬件是 ESP32-S3R8/8 MB flash（非 Adafruit Feather，新增 §6.8）；`HTTPClient` 默认响应超时是 5 s 而非 30 s；补记 `setHandshakeTimeout()` 参数单位为秒。Flash 预算改写：4 MB 分区表锁住了 app 上限，不是芯片不够。TFT_eSPI 配置已收进仓库 |
| 2026-09-19 | 联网模块落地：并发模型按方案 1 实现（`NetworkConfig.h` / `WiFiManager` / `TelemetryClient`，静态内部栈 + portMUX 快照）。**Flash 33.1% → 80.4%**（非空凭据 82.9%），RAM 12.1% → 23.6%。凭据注入两条路径实测通过；§9 增补固件联网坑表 |
| 2026-09-19 | 切 8 MB 分区表（`tinyuf2-partitions-8MB.csv` + `flash_size = 8MB`）：app 槽 1,408 KB → **2048 KB**，Flash 占用 80.4% → **55.3%**（非空凭据 57.0%），余量 17% → 43%。OTA 双分区保留；`nvs`/`otadata` 偏移未变，`ota_1` 后移 |
| 2026-09-19 | **首次实机烧录并验证**（§7.2）：日志证明 `flash 8388608 B`、`psram 0 B`、`netTask on core 0 (internal=1)`、30 s 无重启/无看门狗。日志通道改回 UART0（`CDC_ON_BOOT=0`）并关闭 1200bps-touch 上传流程，顺带省 Flash 36.8 KB / RAM 18.3 KB（现在 53.6% / 17.9%）。**撤销**上一行的“切表后需整片擦除”：实测 PlatformIO 会重写 otadata，无需擦除 |