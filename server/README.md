# Incubator telemetry server

服务器端：接收培养箱上报、持久化、提供只读看板。**固件不在本目录**（固件在 `../src/`，联网改造尚未开始）。
两者与本文冲突时，**以本文与代码为准**（本文是这几份文件里唯一经过实测的）。

---

## 1. 数据流

```
ESP32-S3 ──HTTPS POST──▶ Nginx :443 (175.24.182.190)
                            ├─ /api/v1/telemetry      反代，无 Basic Auth，Bearer 鉴权 + 限流
                            └─ /                   反代 + Basic Auth（看板与查询接口）
                                   ↓
                            Uvicorn/FastAPI  127.0.0.1:8000   ← 绝不监听公网
                                   ↓
                            SQLite /var/lib/incubator/data.db
```

**入口是裸 IP，不用域名。** 原因：`*.lxc-away.site` 未完成 ICP 备案，被腾讯云在网页层按
`Host`/`SNI` 全网拦截；实测用 IP 直连返回 200。因此证书是**自签 CA 签发的
IP 叶子证书**，SAN 必须含 `IP:175.24.182.190`。

---

## 2. 目录

| 路径 | 说明 |
|---|---|
| `app.py` | 全部服务端逻辑（FastAPI 路由、校验、鉴权、告警、查询） |
| `schema.sql` | 三张表 + 索引；每条语句幂等，可直接对已存在的库重复执行 |
| `static/index.html` | 只读看板。**零外部依赖**（不引 CDN、不引图表库），手写 canvas 画曲线 |
| `tests/test_api_smoke.py` | 契约冒烟测试。只用标准库 + 真实 uvicorn 子进程，45 项断言 |
| `deploy/setup.sh` | 幂等安装脚本（见 §4） |
| `deploy/incubator-api.service` | systemd 单元，只绑 `127.0.0.1:8000` |
| `deploy/incubator-https.conf` | Nginx 443 前端的**独立新文件**，不修改既有配置 |

服务器上的落点：

```
/opt/incubator/app/            venv + app.py + schema.sql + static/ + tests/
/var/lib/incubator/data.db     SQLite（WAL）
/etc/incubator/devices.json    设备 Token（640 root:incubator）
/etc/incubator/certs/          自签 CA 与 IP 叶子证书（*.key 600）
/etc/nginx/incubator.htpasswd  看板 Basic Auth（640 root:<nginx user>）
/etc/nginx/conf.d/incubator-https.conf
/etc/systemd/system/incubator-api.service
```

---

## 3. 接口

| 方法 | 路径 | 鉴权 | 说明 |
|---|---|---|---|
| POST | `/api/v1/telemetry` | 设备 Bearer | 上报。成功 `204` 空体 |
| GET | `/api/v1/devices` | Basic Auth | 设备列表 + 在线状态 + 样本数 |
| GET | `/api/v1/devices/{id}/latest` | Basic Auth | 最新一条 + 派生字段 + 未恢复告警 |
| GET | `/api/v1/devices/{id}/history?hours=&step=` | Basic Auth | 曲线数据。`hours>6` 自动按 60 s 分桶 |
| GET | `/api/v1/devices/{id}/alerts?hours=` | Basic Auth | 告警历史 |
| GET | `/healthz` | Basic Auth | 健康检查（在 Nginx 后需密码；`127.0.0.1` 上直连不需要） |

`/docs`、`/redoc`、`/openapi.json` **已主动关闭**：裸 IP 无域名遮蔽，少一个暴露面。

### 上报载荷

`Content-Type: application/json` + `Authorization: Bearer <token>`。`device_id` 必须与 Token 归属一致（否则 `403`）。

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

修正了 4 处（原四项缺口）并新增两项：

| 字段 | 为什么必须有 |
|---|---|
| `temperature_ok` / `humidity_ok` / `co2_ok` | 否则看板无法区分「传感器故障」与「读数恰为 0」 |
| `co2_available` | 与 `co2_ok` 不同：`SystemState.co2Available` 表示传感器在不在线，`co2Valid` 表示本次读数可不可信。`ValveController` 对两者分别处理 |
| `heater_on` | `heater_on=true` 且 `heater_pwm=0` 表示「开关 ON 但传感器故障暂停输出」，与「用户主动关加热」含义完全不同 |
| `uptime_s`（不是 `timestamp`） | 设备不需要 NTP，服务器用接收时刻打权威时间戳 |
| `heap_free` / `heap_min` / `heap_int` | `heap_min` 是自启动以来最低水位，`heap_int` 是**仅内部 RAM**。见 §7 |

**未知字段被忽略（不是报错）**：固件新增字段时，旧服务器不会 400 掉整条样本。
代价是字段名写错会变成 NULL（看板上可见），而不是整条丢失（不可见）。

---

## 4. 部署（手动模式）

把整个 `server/` 打包上传到服务器（宝塔文件管理器），解压到 `/tmp` 下任意位置，然后：

```bash
# 第一步：只装 API，绑 127.0.0.1，不碰任何公网端口
sudo bash /tmp/<dir>/server/deploy/setup.sh

# 验证通过后，第二步：加自签证书 + Nginx 443
sudo bash /tmp/<dir>/server/deploy/setup.sh --with-nginx
```

**为什么分两步**：第一步零公网风险（443 不会被打开），且用 45 项契约测试确认 API 真的对。
第二步才动 Nginx，只**新增**一个 `conf.d/incubator-https.conf`，并在 `nginx -t` 失败时**自动删除自己**再退出。

脚本是幂等的，可重复执行：Token、htpasswd、证书、数据库都**不会**被覆盖，已存在的会被保留。
代码文件（`app.py`/`static/`/`tests/`）每次都会更新。

### 安装脚本会写这些位置

`/opt/incubator/app`、`/var/lib/incubator`、`/etc/incubator`（含 `certs/`）、
`/etc/nginx/incubator.htpasswd`、`/etc/nginx/conf.d/incubator-https.conf`、
`/etc/systemd/system/incubator-api.service`。除此之外只做 `systemctl daemon-reload`、
`systemctl enable/restart incubator-api` 和（第二步的）`systemctl reload nginx`。

**不会碰**：`/etc/nginx/conf.d/ota.conf`（`/fw/` OTA 服务）、`incubator.conf` 的 80 端口块、
`/root`、`/www/server/panel`。

### 回滚

```bash
# 撤掉 443 前端（安全且立即生效）
sudo rm /etc/nginx/conf.d/incubator-https.conf && sudo nginx -t && sudo systemctl reload nginx

# 完全停用
sudo systemctl disable --now incubator-api
```

---

## 5. 运维

```bash
journalctl -u incubator-api -f              # 实时日志
journalctl -u incubator-api --since -1h     # 最近一小时
systemctl restart incubator-api             # 重启

# 改设备显示名（看板上的 label）
sudo nano /etc/incubator/devices.json       # 无需重启：服务按 mtime 自动重载

# 看数据库
sudo -u incubator sqlite3 /var/lib/incubator/data.db \
  "SELECT datetime(recv_ts,'unixepoch','+8 hours') t, temperature, heater_pwm FROM telemetry ORDER BY recv_ts DESC LIMIT 10;"

# 备份（数据库是唯一有状态的资产）
sudo systemctl stop incubator-api
sudo cp /var/lib/incubator/data.db /root/incubator-$(date +%F).db
sudo systemctl start incubator-api

# 手工重跑契约测试
sudo -u incubator /opt/incubator/app/venv/bin/python /opt/incubator/app/tests/test_api_smoke.py
```

**必须离线备份 `ca.key`**（`/etc/incubator/certs/incubator-ca.key`）：它是以后不重烧固件就能换服务器证书的
唯一凭据。**绝不进仓库、绝不进固件**（固件只需要 CA 公钥证书 `incubator-ca.pem`）。

---

## 6. 数据量与留存

10 s 一条、单设备：1 天 8,640 行（约 1 MB）、1 月约 26 万行（约 30 MB）、1 年约 315 万行（约 350 MB）。
服务器磁盘余量紧张（实测约 8.3 GB），因此**留存清理尚未实现**（属 P3）。
在那之前请留意 `du -sh /var/lib/incubator`。

---

## 7. ⚠️ CO₂ 单位：未确认，看板不做换算

`Config.h` 里有 `constexpr uint8_t Co2ScaleFactor = 10;`，注释是
`Preserves the scaling used by the source project. Set to 1 for direct S8 ppm.`
而 `SenseAirS8Sensor.cpp` 实际执行的是：

```cpp
const uint32_t scaledPpm = value * Config::Co2ScaleFactor;   // value = S8 Modbus 寄存器原值
state.setCo2(scaledPpm, true, true);
Serial.printf("[CO2] Reading OK: %lu ppm ...", scaledPpm);   // 打印成 ppm，但已经 ×10
```

`DisplayManager.cpp` 又用 `"ppm"` 作为单位标签显示 `state.co2`。因此：

* `SystemState.co2` = **S8 寄存器原值 × 10**，其真实物理单位取决于 S8 的量程配置（只有硬件能确认）。
* **原始设计**中那句「阀门阈值 25000/40000/48000 对应 2.5%/4.0%/4.8%」**建立在一个未经验证的 ppm 假设上**，
  按代码推论很可能是 10 倍偏差。这是固件侧问题，已记为待确认项。

**本服务的处理**：不对 `co2` 做任何换算，原样存储、原样显示（与设备屏幕一致），
并且**不实现任何 CO₂ 阈值告警**——避免上线一个基于错误单位的报警。
确认单位后再加，改动点只有一处（`app.py` 的 `record_alerts`）。

---

## 8. 安全要点（裸 IP 场景下比域名要求更高）

* 设备 Token：`hmac.compare_digest` 常量时间比较，且**不提前退出**（响应时延不泄露哪个 token 命中）
* 看板：Basic Auth **强制**；`.htpasswd` 放 `/etc/nginx` 而非 `/etc/incubator`，因为后者是
  `0750 root:incubator`，nginx worker（`www-data`）读不到会产生**难以定位的 403**
* `/api/v1/telemetry`：`limit_req 20r/m` + `burst=10` + `client_max_body_size 4k` + `limit_except POST`
* Pydantic 范围校验 + 字段长度上限（`device_id` ≤64、`firmware_version` ≤32）
* FastAPI 只绑 `127.0.0.1`；`/docs` 全关
* `devices.json`、`certs/*.key`、`secrets.ini`、`src/ServerCaCert.h` 全部不进仓库

---

## 9. 已知待办

| # | 项 | 归属 |
|---|---|---|
| 1 | CO₂ 真实单位确认，据此修正原始设计中的 ppm 假设与是否加阈值告警 | 固件/硬件 |
| 2 | 留存清理 `retention.py`（90 天 + 小时聚合） | P3 |
| 3 | 看板的 `co2_pct` 换算显示（等 #1 确认） | P3 |
| 4 | 固件联网模块（并发模型方案 1/2 尚未裁决） | 固件 |
| 5 | ICP 备案后可切域名 + LE 证书（**可选**，只为消除浏览器证书警告） | 可选 |
