# 相机端工具

已实现 USB 单张采集模块、树莓派通信客户端、配置模板、systemd 部署材料和开发模拟器；公网实机联调尚未完成。模拟器将指定的测试图片当作一次拍摄结果上传，控制台明确标记 `SIMULATOR`。

## 树莓派通信客户端

`client.py` 兼容 Python 3.7，通过 `websockets==11.0.3` 接收指令，通过标准库 `urllib` 上传 JPEG。使用已配置的系统 OpenCV 虚拟环境，在相机程序目录执行：

```bash
~/incubator/.venv-camera/bin/python -m pip install -r requirements-pi.txt
read -r -s -p '相机 Token: ' INCUBATOR_CAMERA_TOKEN
export INCUBATOR_CAMERA_TOKEN
~/incubator/.venv-camera/bin/python client.py \
  --server https://175.24.182.190 \
  --ca "$HOME/incubator-ca.pem" \
  --temp-dir "$HOME/incubator/camera-tmp"
```

运行需同时复制 `client.py` 和 `usb_capture.py`。Token 应对应服务器 `camera-001`；服务器相机模块、相机设备配置和 Nginx 通道准备完毕后才能进行真实接入。2026-10-19 用户已确认服务器尚未部署相机功能，当前不能据此命令认定公网拍照可用。前台可用 Ctrl+C 停止。

客户端收到 `connected` 后发送 `hello` 并等待确认，每 10 秒发送真实忙状态。收到拍照指令后分别等待 `capturing` 和 `uploading` 确认，再执行采集和单次上传；成功由服务器保存后决定，客户端不发送 `completed`。忙时拒绝新指令，不排队。

上传在后台线程中执行，不阻塞控制消息；普通断线每隔 5 秒重连。旧任务不借新连接继续发送状态或开始上传；已经发出的 HTTP 请求等待实际结束，本地仍保持忙状态，不重新上传。停止程序时同样等待已发出的上传结束，再清理文件。上传网络超时为 60 秒，并非整个任务的严格总时限。

临时目录应只用于本程序。启动只清理其中 `capture-*.jpg` 遗留文件；每次任务使用随机临时文件名，成功或失败后均删除。不要同时运行两个客户端或预览程序占用摄像头。

配置也可通过环境变量传入，命令行参数优先：

| 环境变量 | 用途 / 默认值 |
|---|---|
| `INCUBATOR_CAMERA_SERVER` | 服务器 HTTP(S) 根地址，必填 |
| `INCUBATOR_CAMERA_TOKEN` | 相机设备 Token，必填 |
| `INCUBATOR_CAMERA_CA` | HTTPS/WSS CA 文件，未设置时使用系统信任库 |
| `INCUBATOR_CAMERA_DEVICE` | OpenCV 设备索引，默认 `0` |
| `INCUBATOR_CAMERA_TEMP_DIR` | 专用临时目录，默认 `/var/lib/incubator-camera/tmp` |

客户端本机测试需启动现有服务器代码，应使用独立的 Python 3.10+ 环境；这不改变树莓派客户端的 Python 3.7 要求。安装 `fastapi`、`uvicorn` 和锁定的相机通信依赖，避免影响使用现代 websockets 的模拟器测试。在仓库根目录运行：

```bash
python -m pip install fastapi uvicorn -r camera/requirements-pi.txt
python -m unittest discover -s camera/tests -p 'test_*.py' -v
```

测试启动临时 FastAPI 后端与本机 HTTP/WebSocket 对端，验证拍照上传和原图下载、失败原因、忙时拒绝、心跳、状态确认、超时、重连与停止清理；OpenCV 使用替身，无需真实设备。实机 Python 3.7 与 USB 摄像头的正式客户端闭环仍待验证。

## 树莓派安装与自启动

以下命令在树莓派的 Bash / SSH 中以已确认的 `pi` 用户执行，适用于当前 Buster、Python 3.7.3 和系统 OpenCV 3.2.0。先把仓库 `camera/` 目录的文件及 `deploy/` 子目录复制到树莓派 `~/incubator/camera/`，保留目录结构。已有 CA 公钥文件位于 `~/incubator-ca.pem`。

按目前服务器实测，相机模块和 Nginx 相机通道尚未部署；`camera-001` Token 也需要在服务器端确认配置。下面先准备树莓派安装文件，待服务器就绪后进行前台拍照上传验证，验证通过再启用自启动。

### 1. 安装程序与依赖

```bash
cd ~/incubator/camera
sudo install -d -o pi -g pi /opt/incubator-camera
sudo install -d /etc/incubator-camera
sudo install -d -o pi -g pi /var/lib/incubator-camera/tmp
install -m 0644 client.py usb_capture.py requirements-pi.txt /opt/incubator-camera/
python3 -m venv --system-site-packages /opt/incubator-camera/venv
/opt/incubator-camera/venv/bin/python -m pip install -r /opt/incubator-camera/requirements-pi.txt
/opt/incubator-camera/venv/bin/python -c "import cv2, websockets; print('OpenCV:', cv2.__version__, 'websockets:', websockets.__version__)"
```

预期导入 OpenCV `3.2.0` 和 websockets `11.0.3`。已有 `~/incubator/.venv-camera` 可继续用于开发试拍；服务使用上面单独创建的固定路径。无需重新安装或编译 OpenCV，也不要求升级系统或全局 Python 依赖。若任一步报错，保留报错并停止后续安装步骤。

### 2. 安装配置和 CA

首次安装执行以下命令；更新程序时不要重复覆盖已填写的配置：

```bash
cd ~/incubator/camera
sudo install -o pi -g pi -m 0600 camera.env.example /etc/incubator-camera/camera.env
sudo install -m 0644 "$HOME/incubator-ca.pem" /etc/incubator-camera/incubator-ca.pem
nano /etc/incubator-camera/camera.env
```

将 `REPLACE_WITH_CAMERA_001_TOKEN` 替换为服务器给 `camera-001` 配置的相同 Token，保留现有 ESP32 的 Token。其余配置沿用模板即可；使用绝对路径，不写 `$HOME` 或 shell 命令。配置模板只包含本客户端已支持的五个变量。

### 3. 前台验证

确保没有预览程序或其他客户端占用相机。服务器部署就绪后，在同一个 Bash 会话中执行：

```bash
set -a
. /etc/incubator-camera/camera.env
set +a
/opt/incubator-camera/venv/bin/python -u /opt/incubator-camera/client.py
```

看到 `connected device=camera-001` 后，在网站点击手动拍照。确认任务完成、照片可查看和下载、`/var/lib/incubator-camera/tmp/` 中照片已清理。仅显示重连日志或 TLS 可连接不算拍照闭环验证通过。通过后按 Ctrl+C 退出，等进程结束，再启动后台服务。

### 4. 启用 systemd

```bash
cd ~/incubator/camera
sudo install -m 0644 deploy/incubator-camera.service /etc/systemd/system/incubator-camera.service
sudo systemd-analyze verify /etc/systemd/system/incubator-camera.service
sudo systemctl daemon-reload
sudo systemctl enable --now incubator-camera
systemctl status incubator-camera --no-pager
journalctl -u incubator-camera -n 50 --no-pager
```

服务以 `pi` 用户运行并使用 `video` 组，开机启动、异常退出后 5 秒重启。Wi-Fi 暂时不可用时由客户端每 5 秒重连；`active` 仅表示进程运行，仍需通过连接日志和网页拍照确认可用。

停止服务时先给主进程发送 SIGTERM，让它终止采集或等待已发出的上传结束并清理照片。等待最多 120 秒后 systemd 会强制结束剩余进程；若因此遗留临时照片，下次启动时清理，不补传。

### 5. 日志、停止与更新

```bash
# 持续查看日志，Ctrl+C 只退出日志查看
journalctl -u incubator-camera -f
# 暂停服务；停用开机启动则使用 disable --now
sudo systemctl stop incubator-camera
```

更新时先停止服务并等命令结束，把新版本相机目录复制到 `~/incubator/camera/`，再执行：

```bash
cd ~/incubator/camera
install -m 0644 client.py usb_capture.py requirements-pi.txt /opt/incubator-camera/
/opt/incubator-camera/venv/bin/python -m pip install -r /opt/incubator-camera/requirements-pi.txt
sudo install -m 0644 deploy/incubator-camera.service /etc/systemd/system/incubator-camera.service
sudo systemd-analyze verify /etc/systemd/system/incubator-camera.service
sudo systemctl daemon-reload
sudo systemctl start incubator-camera
journalctl -u incubator-camera -n 50 --no-pager
```

更新保留 `/etc/incubator-camera/camera.env` 和 CA。树莓派与服务器分别更新。要再次前台试跑，先停止后台服务；确认前台进程结束后再启动服务。开机自启动、异常重启和 Wi-Fi 恢复仍需在树莓派实测，本机配置检查不能代替这些验收。

## 树莓派 USB 单张采集

`usb_capture.py` 使用已有 OpenCV，兼容 Python 3.7，不需要桌面窗口。默认设备索引为 `0`，沿用摄像头默认分辨率及曝光。将该文件复制到树莓派后，在文件所在目录执行：

```bash
~/incubator/.venv-camera/bin/python usb_capture.py --output /tmp/incubator-usb-test.jpg
```

成功返回 `captured_at`（读取照片帧时的 Unix 时间），退出码为 `0`，图片保留供检查。输出文件必须是尚不存在的 `.jpg` 或 `.jpeg`；重复测试前可改用新的文件名。该命令只拍照，不连接服务器。

父进程默认在 30 秒后终止卡住的采集子进程，等待其退出并删除残留文件；可用 `--timeout` 调整。正常路径在成功、读取失败、编码失败时均释放摄像头；强制终止时由操作系统关闭子进程的设备句柄。

后续客户端可调用 `await capture_photo(output, device=0, timeout=30)` 获得拍摄时间。成功照片由调用者在上传结束后删除；采集失败或取消时由模块删除残留文件。采集不阻塞客户端事件循环。

本机无相机测试（标准库即可运行）：

```powershell
python -m unittest discover -s camera/tests -p test_usb_capture.py -v
```

测试用替身 OpenCV 验证独立子进程、失败清理、超时、取消和事件循环响应性，不代表实际 USB 驱动已验收。用户此前已在树莓派验证无窗口 JPEG 拍照；本模块仍需复制到实机验证。

## 本地验证

在已安装 `fastapi`、`uvicorn`、`websockets >= 14` 的 Python 环境中，从仓库根目录运行：

```powershell
python server/tests/test_camera_capture.py
```

测试会创建临时数据库、测试 Token 和真实本机 HTTP/WebSocket 服务，运行模拟相机，验证成功上传、原图读取、忙和离线拒绝、错误上传、断线及临时文件清理。不会连接真实服务器或摄像头。

## 手动运行模拟器

先初始化本地 SQLite 数据库并配置 Token 文件，步骤见 `server/README.md` 的相机手动拍照章节。启动本地服务器后：

```powershell
$env:INCUBATOR_CAMERA_TOKEN = '<camera-001 对应的测试 Token>'
python camera/simulator.py --server http://127.0.0.1:8000 --image C:/path/to/test.png
```

可加 `--capture-delay 2` 模拟耗时拍摄。HTTPS 服务器使用自签 CA 时传入 `--ca C:/path/to/incubator-ca.pem`，不关闭证书校验。

模拟器先发送 `hello`，随后每 10 秒报告心跳及忙状态；接收 `capture` 后依次报告 `capturing` 和 `uploading`，复制测试图片到临时目录并上传。无论上传成功或失败，都删除该临时副本，不修改原测试图片，不重试上传。模拟器断线后退出，不自行重新执行旧任务。

systemd 部署文件已提供；服务器相机通道部署与公网实机联调在后续开发项中完成。
