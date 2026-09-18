# AGENTS.md — 项目结构维护规则

> **本文件是所有 AI harness（Pi / Codex / DSH / Cursor / Copilot 等）在本仓库工作前必须读的规则。**
> 权威分工：**技术决策与项目上下文以 `HANDOFF.md` 为准；仓库结构与版本库操作以本文件为准。**
> 二者冲突时，业务/技术方案听 `HANDOFF.md`，"能不能改这个文件、怎么提交" 听本文件。
> 修改本文件本身需要使用者明确同意。

---

## 0. 三条最重要的

1. **不确定就先问，不要"顺手改一下"。** 尤其涉及配置、目录、依赖、版本历史时。
2. **一个提交只做一件事**，且每个提交点都能 `pio run` 通过。
3. **仓库是 private 且文档含服务器情报**（IP、域名、Nginx 布局），任何扩大可见范围的动作都是不可逆的。

---

## 1. 文件所有权分级

### 🟢 可以自由修改（仍需自证，见 §7）

- `src/*.cpp`、`src/*.h` 的**实现细节**（不改公开接口签名、不改常量值）
- `test/` 下的测试代码
- 注释、日志文本、错误消息措辞

### 🟡 改前必须先问使用者

| 文件 | 为什么 |
|---|---|
| `src/Config.h` | 引脚分配、时序、**PID 增益**、阈值都在这。改错=硬件行为变化 |
| `platformio.ini` | 依赖、工具链、分区表。新增/升级库一律要先问 |
| `src/SystemState.h` | 跨模块数据契约，联网模块依赖它 |
| `.gitignore`、`.gitattributes` | 结构不变量的执行机制本身 |
| `HANDOFF.md`、`docs/` | 交接文档的准确性是另一个会话的唯一上下文来源 |
| 任何**新文件、新目录** | 先说明放哪、为什么、谁维护 |

### 🔴 禁止（无例外，除非使用者在本轮明确授权）

- 修改仓库结构：新增顶层目录、重命名/移动 `src/` 模块、拆分仓库
- 版本历史与分支：见 §2 清单
- 把仓库可见性、remote URL、协作者权限、GitHub 设置做任何改动
- 把**任何二进制文件**加入版本库（png / jpg / pdf / xlsx / .bin / .a）
- 把服务器 IP、域名、Nginx 配置路径、账号、路径信息写进 **`src/` 代码或注释**（只允许出现在 `HANDOFF.md` 与 `docs/`）
- 提交 `secrets.ini`（联网改造阶段引入，含 Wi-Fi 密码 / DeviceToken / CA）

---

## 2. 禁止执行的命令

以下命令**一律先停下来问**。它们造成的后果本地无法回退，或回退代价远高于问一句。

**破坏工作树 / 索引**

```
git clean -fdx          # 会连未跟踪文件一起删（当前所有文件都已入库，但仍别养成习惯）
git reset --hard        # 丢弃未提交改动
git checkout -- <path>  # 丢弃在途改动 ← HANDOFF §8 明令禁止过
git restore, git stash drop / clear
git rm --cached -r .
```

**破坏历史 / 远端**

```
git push --force        # 连 --force-with-lease 也算，都需要授权
git filter-repo / BFG   # 改写历史；本轮 R4 做过一次，代价是全部哈希失效 + 所有 clone 作废
git rebase, git commit --amend（已推送的提交）
git branch -D, git tag -d, git worktree remove --force, git gc --prune=now
git reflog expire
git remote remove / set-url
```

**文件系统**

```
Remove-Item -Recurse -Force  /  rm -rf   指向仓库根或 .git 的路径
```

**凭据 misuse（本轮真实暴露的敞口）**

```
git credential fill   # 能取出 GCM 里保的 token，它有 repo + workflow 写权限
```

- 拿它去调 GitHub API 做**仓库管理**（改可见性、删远端分支、加协作者、建 release）一律**先问**。
  本轮 R2 的 public→private 是使用者明确授权后做的，**不代表默认允许**。
- 允许用途：仅完成使用者本轮明确要求的 git 操作（push/fetch）。
- 不得把 token 输出、写入文件、放进命令行参数或命令返回信息。

**`git add` 纪律：禁止 `git add -A` / `git add .`** —— 必须显式列出路径。
本轮 R2 就是因为工作树同时压着三批改动，`add -A` 会把无关内容卷进提交。

---

## 3. 提交规范

- **一个提交一个主题**。若一个文件里混了两批改动（先例：`HeaterController.cpp` 同时含"加热策略"与"Sht 常量改名"），
  **必须拆开**：临时回退无关的那一行 → 提交 A → 恢复 → 提交 B。
- **每个提交点都必须可编译**。拆完之后，中间提交也要能 `pio run` 通过，不能只验证最后一步。
- **提交前扫描敏感信息**，命中即停：
  ```powershell
  # 1) 凭据形态
  git diff --cached | Select-String -Pattern 'BEGIN .*PRIVATE KEY|ghp_|github_pat_|AKIA|password\s*='
  # 2) 路径白名单判定（比把 IP 抄进命令更可靠）
  git diff --cached --name-only | Select-String -NotMatch '^(HANDOFF\.md$|docs/)'
  ```
  规则：**服务器 IP / 域名 / Nginx 路径只允许出现在 `HANDOFF.md` 与 `docs/`**。
  第二条命令的含义是：只要待提交集里出现了白名单**以外**的文件，就人工核对这些文件里有没有情报。
  本文件故意**不写具体 IP**，避免自己成为泄露源。
- **提交消息风格**：与历史保持一致 —— 英文、祈使句、无句号、50 字符内（例：`Replace AHT20 with SHT30 for temperature and humidity`）。
  需要解释时加正文段落，说明"为什么"而不是"改了哪几行"。
- **不同会话/harness 的改动不要混进同一个提交**，并在正文里注明来源。
- 正文里标注**未验证项**（例：`STATUS: work in progress, not yet build-verified`），不要让读者猜。
- 不要 `git push`，除非使用者要求（见 §6 的网络前提）。

---

## 4. 行尾、编码与格式

- **全部 LF，UTF-8 无 BOM。** 已由 `.gitattributes`（`* text=auto eol=lf`）强制，它**优先于** `core.autocrlf`。
- **不要**依赖 `core.autocrlf`（本机是系统级 `true`，不进仓库、对协作者不可见）。
- 不要用会写入 CRLF 或 BOM 的方式落盘（PowerShell `Set-Content` / `>` 重定向默认带 BOM，用 `[IO.File]::WriteAllText` + `UTF8Encoding($false)`）。
- **`.sh` / `.py` 必须是 LF** —— CRLF 的 shell 脚本在 Ubuntu 服务器上会报 `bash: $'\r': command not found`。
- C++ 代码沿用现有风格：2 空格缩进、`#pragma once`、私有成员尾 `_`、`Config::` 常量 PascalCase。

---

## 5. 结构不变量（不要打乱这些）

```
src/            一个模块 = 一对 .h/.cpp，PascalCase 文件名与类名（现有 11 个模块）
include/ lib/   PlatformIO 约定目录，目前为空，不要往里塞东西
test/           测试
docs/           设计文档（唯一允许的长期文档存放处）
tmp/            已被 .gitignore 忽略；临时材料、原理图截图、参考 PDF 全放这里
platformio.ini  唯一构建配置，env 只有一个：adafruit_feather_esp32s3
HANDOFF.md      交接文档，仓库根目录，便于任何会话第一时间发现
AGENTS.md       本文件：结构维护规则（🟡 修改需使用者同意）
```

- 新模块放 `src/`，**不要在根目录建新目录**。
- 新代码遵循既有分层：`main.cpp` 只做装配与 `setup/loop`，逻辑在各 Controller/Sensor 类里，状态集中在 `SystemState`。
- **联网功能不得阻塞主循环**（`HANDOFF.md` §8 红线）；Wi-Fi/服务器不可用时本地 PID、阀门、风扇必须照常运行。
- 加依赖前先确认现有库能否解决（当前仅 `SHT85` / `TFT_eSPI` / `PID` 三个外部库 + Arduino 自带 `Wire`/`Preferences`）。

---

## 6. 环境与网络事实（省得重复排查）

| 事实 | 影响 |
|---|---|
| `github.com:443` **直连超时**；`api.github.com:443` 可直连 | `git push/fetch` 必须走代理 |
| 本机 `127.0.0.1:7890` 有代理在监听，但系统 `ProxyEnable=0` | git 拿不到，需手动指定 |
| 使用者明确要求 **`http.proxy` 不持久化** | 每次网络命令**自带**：`git -c http.proxy=http://127.0.0.1:7890 push origin master` |
| `pio` 不在 PATH | 用完整路径：`C:\Users\Emila_58035\.platformio\penv\Scripts\platformio.exe run` |
| Windows PowerShell 5.1 默认不启用 TLS 1.2 | 调 GitHub API 前先 `[Net.ServicePointManager]::SecurityProtocol = 'Tls12'`，否则报"基础连接已经关闭" |
| 历史改写（R4）前的完整备份 | `D:\PlatformIO\Cell\_backups\Cell_incubator-20260918-2110-*`（mirror + bundle），**不要删** |

---

## 7. 改完必须自证（不接受"应该可以"）

| 动了什么 | 必须提供的证据 |
|---|---|
| 任何 `src/` 改动 | `pio run` 的 **SUCCESS** 原文（含 RAM/Flash 数字） |
| 拆分/重写提交 | 对**每个**中间提交单独检出编译（`git worktree` 是安全做法），不能只测 HEAD |
| 改动结构/行尾/索引 | `git status --short`、`git ls-files --eol`、`git diff --stat` |
| 改写历史 | ①先做 `git bundle`/mirror 备份并 `bundle verify` ②新旧 HEAD `ls-tree -r` 逐文件比对差异为 0 ③提交数差异有解释 ④`git fsck` 无错 |
| 声称某事已完成 | 给命令与输出，不要凭印象。**文档里写的数字/结论都要有当次实测支撑** |

失败或没验证就**明说没验证**，并把它列成待办。

---

## 8. 文档同步义务

改完版本库/分支/依赖结构后，**必须同步更新 `HANDOFF.md`** 对应小节，
否则下一个会话会基于过期事实行动（本轮 R2/R3 就各出现了一批这样的过期陈述）。

- 新增结构性操作 → 在 `HANDOFF.md` §4.4 表格加一行，必要时开新小节
- §5.1 时间线加一行
- **不要在文档里写死 HEAD 哈希**（R4 改写后全部失效，教训已记在 §4.6）
- 文档中的"未完成/未验证"结论必须当次实测复核，不要沿用

---

*最后更新：2026-09-18（R4 之后）。*
