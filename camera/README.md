# 相机端工具

已实现 USB 单张采集模块、树莓派通信客户端和开发模拟器；systemd 部署材料尚待实现。模拟器将指定的测试图片当作一次拍摄结果上传，控制台明确标记 `SIMULATOR`。

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

systemd 自启动与公网实机联调在后续开发项中完成。
