# 相机端工具

已实现 USB 单张采集模块和开发模拟器；树莓派正式通信客户端、systemd 部署材料尚待实现。模拟器将指定的测试图片当作一次拍摄结果上传，控制台明确标记 `SIMULATOR`。

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

正式客户端、systemd 自启动与断线重连在后续开发项中实现。
