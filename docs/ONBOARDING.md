# 协作者接入指南

> 本仓库是 **private**，`docs/` 下含服务器 IP、Nginx 布局与部署脚本。
> **被加入 = 你拿到了全部提交历史与这些情报**，而且你 clone 的副本事后不会被收回。

## 1. 接受邀请

仓库所有者会在 GitHub → Settings → Collaborators → Add people 发出邀请。
接受邮件后你就有 Write 权限，可以直推 master。

⚠️ 免费档 private 仓库不支持分支保护，所以“直推 master”没有技术闸门——下面第 5 节的约定就是防线。

## 2. 环境准备

| 项 | 说明 |
|---|---|
| Git | 命令行或任意客户端 |
| VS Code + PlatformIO IDE 扩展 | 仓库自带的 `.vscode/extensions.json` 在打开时会提示安装 |
| clone | `git clone https://github.com/emila58035/Cell_incubator.git` |
| 首次构建 | `pio run` 会从 GitHub 拉取钉住的 `arduino-esp32 3.0.7` 与 `esp32-arduino-libs`，中国大陆直连常超时，需自备可用网络 |
| 行尾 | `.gitattributes` 强制 LF / UTF-8 无 BOM，不要改 `core.autocrlf` |

## 3. 拿不到的三个文件（必须带外传）

它们被 `.gitignore` 排除，不会随仓库到达。缺了仍能编译，但设备不会联网。

| 文件 | 内容 | 怎么拿到 |
|---|---|---|
| `src/Secrets.h` | Wi-Fi SSID/密码、服务器 URL、设备 ID、设备 Token | 由所有者通过仓库外渠道发给你，不要写进 issue / PR / 提交消息 |
| `src/ServerCaCert.h` | 自签 CA 的**公钥**证书 | 取服务器上的 `incubator-ca.pem`（位置见 `docs/REMOTE_ACCESS.md` §2.4）；**私钥永不出服务器** |
| 看板 Basic Auth 密码 | 浏览器看数据用 | 所有者单独给 |

设备 Token 存于服务器 `/etc/incubator/devices.json`，格式见 `docs/REMOTE_ACCESS.md` §2.4。

## 4. 首次烧录

- **必须擦除整片 flash**：分区表已换成 8 MB，残留的 `otadata` 会指向旧槽。
- `pio run -t upload --upload-port <COM口>`（COM 口每台机器不同）
- **`pio run -e sim` 绝不能烧到接了加热/阀门的板子上**：模拟器喂合成温度，PID 驱动真实输出。
- 更多细节见 `README.md` 的“使用方法”一节。

## 5. 协作约定

- **一个提交一个主题**；动了 `src/` 必须 `pio run` 通过再提交（改过 `src/` 建议两个 env 都编）。
- 结构规则见 `AGENTS.md`——若你用 AI 编码工具，它会被自动注入。
- 服务器 IP / 域名 / Nginx 路径**只允许出现在 `docs/`**；写进 `src/` 或顶层文件就是错误。
- 凭据永不进仓库、不出现提交消息或 issue。
- **不 force-push、不改写已推送的历史**；确需改写先说明并取得同意。

## 6. 如果还拿到了服务器 SSH

那台机器**不是本项目专用机**：上面跑着与本项目无关的长期服务。
**动手前必读** `docs/REMOTE_ACCESS.md` §2.5「机器上的既有服务与红线」：
那里列了不得触碰的路径、不得使用的命令（含会连带破坏既有 OTA 站点的证书工具用法）、
面板配置为何不生效、以及磁盘紧张时的正确处理方式。

数据库是唯一有状态的资产，改前先备份（同文档 §4.9）。

## 7. 退出

所有者移除你的权限后，你已 clone 的副本仍在你本地存在，
但不会再收到新的提交。如果怀疑凭据泄露，比移除权限更紧迫的是**轮换设备 Token 与看板密码**。