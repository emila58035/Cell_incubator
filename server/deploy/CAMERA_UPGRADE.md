# 现有服务器相机功能升级

适用于已运行 `incubator-api` 和 HTTPS 入口的 `ubuntu@175.24.182.190`。2026-10-19 实测服务器尚无 `camera_api.py` 和相机 Nginx 通道；本文是待执行步骤，不代表已部署。首次安装仍可使用 `setup.sh`，现有服务器按本文升级，不直接运行 `setup.sh --with-nginx`：旧脚本在校验失败时会删除目标配置，不能恢复已有 HTTPS 文件。

本机已检查代理路由范围、大小及路径一致性，遥测接口、ESP32 WebSocket 远控和相机拍照 41 项检查通过。本机为 Windows，未执行 Linux 的 `nginx -t`、`systemd-analyze verify` 或本文 Bash 部署命令；以下服务器检查与实机验收仍需执行。

## 1. 上传与准备

在本机仓库根目录的 PowerShell 执行，将当前 `server/` 上传到独立暂存目录：

```powershell
ssh ubuntu@175.24.182.190 "mkdir -p ~/incubator-camera-release"
scp -r .\server ubuntu@175.24.182.190:~/incubator-camera-release/
```

随后 SSH 登录服务器。以下步骤都在服务器的同一个 Bash 会话执行；任一步报错先停止并保留报错。无需修改证书、现有 ESP32 Token、80 端口和 OTA 配置。

```bash
cd ~/incubator-camera-release/server
/opt/incubator/app/venv/bin/python --version
/opt/incubator/app/venv/bin/python -c 'import fastapi, uvicorn, websockets; print(fastapi.__version__, uvicorn.__version__, websockets.__version__)'
sudo nginx -t
```

服务端要求 Python 3.10+，继续使用现有服务器虚拟环境，不能改用树莓派的 Python 3.7 或依赖版本。以上命令异常时先反馈，不执行后续覆盖步骤。先暂停网站中可能已启用的间隔拍摄；重启会恢复启用配置并立即尝试一次。

## 2. 备份与更新 API

先停服务再复制数据库，避免漏掉 SQLite WAL 中的数据。备份包含数据库、照片、设备配置、程序（不含虚拟环境）、服务单元和现用 HTTPS 配置。停服会暂时中断遥测接收与远控。

```bash
CAMERA_UPGRADE_BACKUP="/var/backups/incubator-camera-$(date +%Y%m%d-%H%M%S)"
sudo install -d -m 0700 "$CAMERA_UPGRADE_BACKUP"
sudo cp -a /etc/nginx/conf.d/incubator-https.conf "$CAMERA_UPGRADE_BACKUP/incubator-https.conf"
sudo cp -a /etc/systemd/system/incubator-api.service "$CAMERA_UPGRADE_BACKUP/incubator-api.service"
sudo cp -a /etc/incubator/devices.json "$CAMERA_UPGRADE_BACKUP/devices.json"
sudo systemctl stop incubator-api
sudo tar --exclude=./venv -C /opt/incubator/app -cpf "$CAMERA_UPGRADE_BACKUP/app.tar" .
sudo cp -a /var/lib/incubator "$CAMERA_UPGRADE_BACKUP/data"
printf '备份目录：%s\n' "$CAMERA_UPGRADE_BACKUP"
```

记下输出的备份目录。更新文件并使用服务账号应用幂等数据库结构：

```bash
sudo install -o incubator -g incubator -m 0644 app.py camera_api.py schema.sql README.md /opt/incubator/app/
sudo install -o incubator -g incubator -m 0644 static/index.html static/camera.html /opt/incubator/app/static/
sudo install -d -o incubator -g incubator -m 0750 /var/lib/incubator/photos
sudo -u incubator /opt/incubator/app/venv/bin/python - <<'PY'
import sqlite3
from pathlib import Path
conn = sqlite3.connect('/var/lib/incubator/data.db')
try:
    conn.executescript(Path('/opt/incubator/app/schema.sql').read_text(encoding='utf-8'))
    conn.commit()
    print('数据库结构更新完成')
finally:
    conn.close()
PY
sudo install -m 0644 deploy/incubator-api.service /etc/systemd/system/incubator-api.service
sudo systemd-analyze verify /etc/systemd/system/incubator-api.service
```

相机 Token 不由安装脚本自动生成。在原设备配置中新增 `camera-001`，保留已有全部条目：

```bash
openssl rand -hex 32
sudo nano /etc/incubator/devices.json
```

用生成值填写新增条目，保持 JSON 格式有效：

```json
"camera-001": {"token": "生成的相机Token", "label": "USB相机"}
```

树莓派 `/etc/incubator-camera/camera.env` 中 `INCUBATOR_CAMERA_TOKEN` 填相同值。若 `camera-001` 已存在，沿用它，不重新生成或替换。

```bash
sudo /opt/incubator/app/venv/bin/python -m json.tool /etc/incubator/devices.json >/dev/null
sudo chown root:incubator /etc/incubator/devices.json
sudo chmod 0640 /etc/incubator/devices.json
sudo -u incubator test -w /var/lib/incubator/photos
sudo systemctl daemon-reload
sudo systemctl start incubator-api
systemctl is-active incubator-api
curl -fsS http://127.0.0.1:8000/healthz
curl -fsS http://127.0.0.1:8000/api/v1/camera/status
curl -fsS http://127.0.0.1:8000/api/v1/camera/schedule
sudo journalctl -u incubator-api -n 50 --no-pager
```

没有树莓派连接时，相机离线是正常结果。相机目录固定为 `/var/lib/incubator/photos`，处于现有服务可写路径内；保持 `--workers 1`，避免重复调度。若有自定义 systemd override，需确认没有覆盖本文的相机路径、大小限制或单实例设置。

## 3. 更新 Nginx

先比较当前配置和仓库模板；若存在本文以外的自定义差异，先反馈差异，不直接覆盖。备份已在第二步创建。

```bash
sudo diff -u /etc/nginx/conf.d/incubator-https.conf deploy/incubator-https.conf
```

`diff` 返回 1 表示有差异，并非部署失败。确认差异后执行：

```bash
sudo install -m 0644 deploy/incubator-https.conf /etc/nginx/conf.d/incubator-https.conf
if sudo nginx -t; then
    if ! sudo systemctl reload nginx; then
        sudo cp -a "$CAMERA_UPGRADE_BACKUP/incubator-https.conf" /etc/nginx/conf.d/incubator-https.conf
        sudo nginx -t && sudo systemctl reload nginx
        printf '重载失败，已恢复原配置；停止后续步骤并检查日志。\n'
    fi
else
    sudo cp -a "$CAMERA_UPGRADE_BACKUP/incubator-https.conf" /etc/nginx/conf.d/incubator-https.conf
    sudo nginx -t
    printf '校验失败，已恢复原配置；未重载，停止后续步骤并检查报错。\n'
fi
```

仅新增两类设备入口：精确的 `/api/v1/camera-stream` 和 `/api/v1/camera/captures/{task_id}/image` 上传路径。它们不要求网页 Basic Auth，由 API 校验相机 Bearer；相机页面、状态、手动拍照、定时配置、照片浏览和删除继续使用网站原入口。

上传大小为 `16m`，对应 API 的 `16777216` 字节，接收和代理读写空闲超时为 60 秒；相机 WS 读空闲超时为 75 秒，客户端每 10 秒心跳，API 45 秒判断离线。API 的任务总期限为 120 秒。空闲超时不等于总期限；上传关闭请求缓冲，API 随接收检查任务期限和大小，不由 Nginx 自动重试。以上均为开发暂定值，需实拍后确认。

## 4. 公网联调与验收

1. 浏览器进入现有网站，确认首页、遥测和 ESP32 远控正常，打开 `/camera.html`。
2. 按 `camera/README.md` 在树莓派前台启动客户端；确认日志显示 `connected device=camera-001`，网页显示相机在线。
3. 手动拍一张：状态依次推进、原图可查看下载、时间正确、附近遥测按 30 秒规则显示，树莓派临时照片删除。
4. 重复请求确认忙时拒绝；开启间隔拍摄，确认立即尝试和后续按间隔触发，再停用定时。
5. 断开客户端，确认网页离线、手动拍照失败；恢复后不重发旧任务。验证下载不删除、勾选删除仅处理所选照片及关联任务。
6. 前台验证通过后启用树莓派 systemd，再验证 Wi-Fi 恢复、停止清理和开机启动。服务器重启恢复规则最后单独验证，记录实际结果后更新方案。

排查时保留 HTTP 状态码和两端日志：`401` 且提示 Basic 表示请求进入网页入口；`403` 需区分 Nginx 方法限制与 API Token 校验；`413` 表示大小限制；`409` 常见于忙或迟到上传。不要关闭 TLS 校验，也不自动重试拍照或上传。

## 5. 回滚

若仅 Nginx 校验失败，第三步会恢复现用文件，API 无需回滚。若升级 API 后启动失败，保持树莓派客户端停止，使用第二步记录的备份目录恢复程序和服务单元：

```bash
sudo systemctl stop incubator-api
sudo tar -C /opt/incubator/app -xpf "$CAMERA_UPGRADE_BACKUP/app.tar"
sudo cp -a "$CAMERA_UPGRADE_BACKUP/incubator-api.service" /etc/systemd/system/incubator-api.service
sudo cp -a "$CAMERA_UPGRADE_BACKUP/incubator-https.conf" /etc/nginx/conf.d/incubator-https.conf
sudo systemctl daemon-reload
sudo systemctl start incubator-api
sudo nginx -t && sudo systemctl reload nginx
```

相机新增表与原表独立，回退旧代码时保留当前数据库、照片和设备配置，不删除升级后收到的遥测。备份中的 `data/` 和 `devices.json` 供需要进一步恢复数据时单独处理；不默认用旧数据覆盖运行后的数据。新 Bash 会话中需先将 `CAMERA_UPGRADE_BACKUP` 设为实际备份目录。
