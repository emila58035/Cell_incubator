# Incubator telemetry server

服务器端：接收培养箱上报、持久化、提供看板和 WebSocket 远程控制。**固件不在本目录**（固件在 `../src/`）。
两者与本文冲突时，**以本文与代码为准**（本文是这几份文件里唯一经过实测的）。

---

## 1. 数据流

```
ESP32-S3 ──HTTPS POST──▶ Nginx :443 (175.24.182.190)
                            ├─ /api/v1/telemetry      反代，无 Basic Auth，Bearer 鉴权 + 限流
                            ├─ /api/v1/device-stream  WebSocket，Bearer 鉴权
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
| `app.py` | 主服务、遥测、鉴权、告警、查询与远程控制 |
| `camera_api.py` | 相机连接、拍照任务、间隔调度、照片与环境关联 |
| `schema.sql` | 三张表 + 索引；每条语句幂等，可直接对已存在的库重复执行 |
| `static/index.html` | 看板与远程控制。**零外部依赖**（不引 CDN、不引图表库），手写 canvas 画曲线 |
| `tests/test_api_smoke.py` | HTTP 契约冒烟测试。只用标准库 + 真实 uvicorn 子进程 |
| `tests/test_websocket_control.py` | WebSocket 连接、命令推送和 ACK 冒烟测试 |
| `tests/test_dashboard_browser.py` | 电脑/手机网页、原始数据分页、缩放、CSV 与模拟设备远控验证；开发时使用 Playwright |
| `deploy/setup.sh` | 幂等安装脚本（见 §4） |
| `deploy/incubator-api.service` | systemd 单元，只绑 `127.0.0.1:8000` |
| `deploy/incubator-https.conf` | Nginx 443 前端的**独立新文件**，不修改既有配置 |
| `deploy/CAMERA_UPGRADE.md` | 现有服务器相机升级、备份、Nginx 校验与恢复、实机验收步骤 |

服务器上的落点：

```
/opt/incubator/app/            venv + app.py + schema.sql + static/ + tests/
/var/lib/incubator/data.db     SQLite（WAL）
/var/lib/incubator/photos/     相机原图（与数据库一并保留）
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
| WebSocket | `/api/v1/device-stream` | 设备 Bearer | 长连接遥测、心跳、命令下发与回执 |
| GET | `/api/v1/devices` | Basic Auth | 设备列表 + 在线状态 + 样本数 |
| GET | `/api/v1/devices/{id}/latest` | Basic Auth | 最新一条 + 派生字段 + 未恢复告警 |
| GET | `/api/v1/devices/{id}/history?hours=&from_ts=&to_ts=&cursor=&snapshot_id=&limit=` | Basic Auth | 所有时间范围默认返回原始采样，按时间和采样 ID 升序分页；显式 `step` 参数可请求聚合 |
| GET | `/api/v1/devices/{id}/alerts?hours=` | Basic Auth | 告警历史 |
| POST | `/api/v1/devices/{id}/commands` | Basic Auth（由 Nginx 强制） | 创建远程控制命令 |
| GET | `/api/v1/devices/{id}/commands/{command_id}` | Basic Auth（由 Nginx 强制） | 查询命令执行状态 |
| GET | `/api/v1/devices/{id}/export.csv?hours=&from_ts=&to_ts=` | Basic Auth | 下载所选范围内的原始温度、湿度和 CO₂ CSV |
| GET | `/healthz` | Basic Auth | 健康检查（在 Nginx 后需密码；`127.0.0.1` 上直连不需要） |

`/docs`、`/redoc`、`/openapi.json` **已主动关闭**：裸 IP 无域名遮蔽，少一个暴露面。

### 新看板与原始历史分页

看板将执行器状态与控制合并为卡片，支持电脑/手机和深浅主题。温度、湿度、CO₂ 为三个独立趋势图，共用时间范围；CO₂ 纵轴固定为 `0–100000`。远控按钮仅在 `ws_connected=true` 时可用，目标温度及亮度输入不会被自动刷新覆盖。

1 小时、6 小时、24 小时、7 天及自定义范围均显示原始采样，不取分钟平均；悬停/触摸查看原始点，拖动框选或使用按钮缩放时间轴。CSV 使用当前查看范围，包括缩放后的范围，时间列为 UTC。

历史接口每页默认 2000 点，`limit` 最大为 5000。第一页返回 `from_ts`、`to_ts`、`snapshot_id`、`last_cursor` 与 `next_cursor`；下一页需重复相同起止时间及快照 ID，并将 `next_cursor` 作为 `cursor`，直到 `next_cursor=null`。时间与 ID 联合游标可保留同一秒内的多条采样。原始浮点值不作二次舍入。显式 `step` 聚合接口保留旧的 2000 点上限，网页不使用该模式。

网页缓存已加载的范围，实时窗口后续只获取末尾游标之后的新点，并移除过期点。自定义范围需同时提供 `from_ts` 和 `to_ts`（Unix 秒），跨度大于 0 且不超过 30 天。7 天按 10 秒采样约 60,480 点，首次加载比短范围耗时更长。

开发验证：

```bash
python -m pip install playwright
python -m playwright install chromium
python server/tests/test_dashboard_browser.py --output /tmp/incubator-dashboard-preview
```

如已安装 Edge/Chrome，可用 `--browser-executable` 指定浏览器路径。浏览器测试使用临时数据库和模拟设备，不接触实际设备；服务器运行无需安装 Playwright。

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

**已有 HTTPS 服务的相机升级请使用 [CAMERA_UPGRADE.md](deploy/CAMERA_UPGRADE.md)**，其中包含备份和恢复步骤。下方是安装脚本原有流程；旧脚本的 `--with-nginx` 在校验失败时会删除目标配置，不能恢复已存在的 HTTPS 文件。

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

# 遥测数据库备份（相机启用后还需同时保留 photos/，见相机升级说明）
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
| 4 | WebSocket 远控实机联调（需上传本目录并烧录对应固件） | 固件/服务器 |
| 5 | ICP 备案后可切域名 + LE 证书（**可选**，只为消除浏览器证书警告） | 可选 |

## 10. 相机拍照与图库（前三项已完成本地模拟验证）

已实现相机独立 WebSocket、手动拍照任务、HTTP 单次图片上传、任务状态查询、原图读取下载、单定时任务、任务超时、服务器重启恢复、图库页面、环境数据关联和批量删除。完整目标见 `../docs/CAMERA_CAPTURE_PLAN.md`。相机 Nginx 通道、照片目录、systemd 参数和升级步骤已提供，尚未部署到公网服务器或进行真实摄像头联调。

新增文件：`camera_api.py`、`tests/test_camera_schedule.py`；数据库新增 `camera_captures`、`camera_photos` 和 `camera_schedule`。更新时需重复应用 `schema.sql`，部署脚本会安装新模块。相机测试依赖仓库中的开发模拟器，从完整仓库运行，不改变仅打包 `server/` 的部署方式。

### 配置

| 环境变量 | 当前默认值 | 用途 |
|---|---|---|
| `INCUBATOR_CAMERA_ID` | `camera-001` | 相机设备标识，必须对应现有 devices.json 中的独立 Token |
| `INCUBATOR_CAMERA_PHOTOS` | 数据库所在目录下的 `photos/` | 原图目录，服务账号需有写权限 |
| `INCUBATOR_CAMERA_OFFLINE_S` | `45` | 心跳离线判断秒数，开发暂定值 |
| `INCUBATOR_CAMERA_MAX_BYTES` | `16777216`（16 MiB） | 单张上传上限，开发暂定值 |
| `INCUBATOR_CAMERA_TASK_TIMEOUT_S` | `120` | 单次拍照任务超时秒数，开发暂定值 |

相机 Token 放入现有设备配置，不写入数据库。不要覆盖原有 ESP32 的 Token。图片暂支持 JPEG 和 PNG，硬件格式尚未确定；只检查文件头匹配声明类型，不修改图片内容、不单独记录尺寸或文件大小。

### 当前通信契约

- WebSocket `/api/v1/camera-stream`：Bearer 必须属于配置的相机设备。
- 服务器先返回 `{"type":"connected","device_id":"camera-001"}`。
- 相机发送 `{"type":"hello","busy":false}` 后才可接受任务；心跳为 `{"type":"heartbeat","busy":false}`，执行中报告 `busy:true`。
- 手动 `POST /api/v1/camera/captures`：成功为 `201`，返回任务编号及 `pending` 状态；离线为 `503`，设备忙或未就绪为 `409`，响应含失败任务编号和原因。
- 服务器下发 `{"type":"capture","task_id":"..."}`。
- 相机发送 `{"type":"capture_status","task_id":"...","status":"capturing"}`，再发送 `uploading`，服务器各返回 `capture_status_ack`；失败时发送 `failed` 和 `reason`。
- 相机单次 `POST /api/v1/camera/captures/{task_id}/image?captured_at=<Unix秒，可带小数>`，Bearer 鉴权，正文为原始图片字节，`Content-Type` 为 `image/jpeg` 或 `image/png`。无需 multipart 依赖。
- 保存成功为 `201`，返回照片编号、拍摄时间、文件名和 `file_url`。服务器保存后才将任务置为 `completed`；相机不能自行报告完成。
- `GET /api/v1/camera/status` 返回在线、就绪、忙状态与当前任务编号；`GET /api/v1/camera/captures/{task_id}` 返回任务状态。
- `GET /api/v1/camera/schedule` 返回定时开关、间隔和下次触发时间；`PUT` 接收 `{"enabled":true,"interval_s":600}`，启用时立即尝试一次，忙或离线记为 `skipped`。
- `GET /api/v1/camera/photos/{photo_id}` 获取照片信息；`GET /api/v1/camera/photos/{photo_id}/file` 显示原图，加 `?download=true` 下载原图。
- `GET /api/v1/camera/photos?page=1&page_size=12` 按拍摄时间、照片编号倒序分页，返回 `items/total/page/page_size`，每页最多 100 张；`GET /api/v1/camera/photos/latest` 返回最新照片信息，无照片返回 JSON `null`。
- 照片详情中的 `environment` 返回单培养箱拍摄前后 30 秒内最近一条遥测（等距取较早记录），包含实际采样时间、温湿度和 CO₂ 及其有效状态；没有匹配时为 `null`。沿用首页设备顺序，排除独立相机设备，不保存环境数据副本。
- `POST /api/v1/camera/photos/delete` 接收 `{"photo_ids":["..."]}`（1–100 项），去重后逐项删除文件、照片和对应任务，返回 `deleted` 编号列表及 `failed` 中的编号和原因；原始遥测不删除。
- 相机状态新增 `latest_task`，用于显示最近任务的完成、失败或跳过原因；当前任务仍可按编号查询。

### 网页使用

首页底部显示最新照片和相机入口。`/camera.html` 支持手动拍照、定时开关与秒数间隔、最新照片、每页 12 张图库、照片详情、附近环境数据和下载原图。勾选照片或全选当前页后，确认一次批量删除；下载不会删除照片。

详情可放大、旋转 90°、水平翻转、按左右或上下两侧对称裁剪。调整仅在浏览器查看中生效，关闭后不保存，下载始终为原文件。图库直接使用原图缩小显示，不生成独立缩略图。页面支持浅色和深色主题，以及手机布局。

任务状态值为 `pending`、`capturing`、`uploading`、`completed`、`failed`、`skipped`。正在执行时不接受第二项任务；重复或迟到上传返回 `409`，不会生成第二张照片。连接正常断开时，未完成任务记录为失败。任务超过 `INCUBATOR_CAMERA_TASK_TIMEOUT_S` 后记录 `capture_timeout`。服务器重启时未完成任务记录为 `server_restarted`，已启用的定时配置恢复并立即尝试一次。照片已保存但 HTTP 回应丢失时，服务器完成状态不会因相机后续失败回报而被覆盖。

任务超时和启动恢复已完成本地模拟验证；设备保持连接但不回报时，服务器会在配置的任务超时后结束任务。暂无自动清理期限。

### 验证

```powershell
python server/tests/test_camera_capture.py
python server/tests/test_camera_schedule.py
python server/tests/test_api_smoke.py
python server/tests/test_websocket_control.py
python server/tests/test_camera_gallery.py
```

图库浏览器测试需要已有 Playwright 和 Chromium；也可指定本机浏览器，例如 `python server/tests/test_camera_gallery.py --browser-executable 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'`。测试使用临时数据库和模拟相机，不操作真实设备。

模拟器使用方法见 `../camera/README.md`。本地手动运行服务器时，将 `INCUBATOR_DB`、`INCUBATOR_DEVICES` 指向测试文件，先用 Python 的 `sqlite3.Connection.executescript` 应用 `schema.sql`，再在 `server/` 目录运行 `python -m uvicorn app:app --host 127.0.0.1 --port 8000`。
