# 相机开发模拟器

当前只实现开发模拟器，尚未实现真实树莓派摄像头驱动。模拟器将指定的测试图片当作一次拍摄结果上传，控制台明确标记 `SIMULATOR`。

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

真实驱动、systemd 自启动与断线重连在后续开发项中实现。
