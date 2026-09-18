# CONVENTIONS —— 操作细节与 recipe

> `AGENTS.md` 是常驻的判级规则，本文件是**按需查的操作细节**。
> 不确定怎么做才来看；知道做什么就别读，直接干。

---

## 1. 开工三查 + 还原点

任何有实质改动的会话，动手前先跑这一条（🟢，1 秒）：

```powershell
git status --short; git log -1 --format='%h %ad %s' --date=iso; git branch --show-current
```

- **工作树不干净时先停下来问**——里面可能是别人（或上一个会话）的在途改动。
  本轮 R2 就碰上工作树同时压着两批改动，`git add -A` 会把无关内容卷进提交。
- 大改前打还原点，把"改乱了"从"需要人拦"变成"一条命令撤销"：

```powershell
git tag -f ai-checkpoint          # 弄砸了就： git reset --hard ai-checkpoint
```

- 涉及历史/远端的动作前，做一次 bundle 备份（`_backups/` 里已有先例）：

```powershell
git bundle create ..\_backups\pre-<动作>-<日期>.bundle --all
git bundle verify ..\_backups\pre-<动作>-<日期>.bundle   # 必须 "is okay"
```

---

## 2. 拆分混装的提交

一个文件里同时有两批不相关改动时（先例：`HeaterController.cpp` 同时含"加热策略"与"`Aht`→`Sht` 常量改名"），**必须拆开**。

### 做法 A：临时回退无关行（适合只有 1–2 行交叉）

```powershell
Copy-Item src/X.cpp tmp/X.cpp.final          # 先存最终版（tmp/ 已被忽略）
# 把无关的那一行改回旧写法（只动那一个 token）
git add src/A.cpp src/B.cpp                  # 只暂存主题一的改动
git commit -m '...'                          # 提交 A
Copy-Item tmp/X.cpp.final src/X.cpp -Force   # 恢复最终版
Remove-Item tmp/X.cpp.final
git add src/X.cpp ...                        # 暂存主题二
git commit -m '...'                          # 提交 B
```

自检：`git diff HEAD~1 -- src/X.cpp` 里**不该**出现主题一的 token。

### 做法 B：每个提交都单独编译（拆完之后必做）

```powershell
git worktree add --detach ../_wt_check <中间提交>
Push-Location ../_wt_check; & "<pio完整路径>" run; Pop-Location
git worktree remove --force ../_wt_check
```

只测 HEAD 不算过——中间提交坏掉会让 `bisect` 和 cherry-pick 失效。

---

## 3. 提交前扫描

```powershell
# 1) 凭据形态 —— 只扫代码与配置
git diff --cached -- src platformio.ini .gitignore .gitattributes |
  Select-String -Pattern 'BEGIN .*PRIVATE KEY|ghp_[A-Za-z0-9]{16}|github_pat_|AKIA[0-9A-Z]{16}|password\s*=\s*\S'

# 2) 路径白名单 —— 出现白名单以外的文件就人工核对有没有情报
git diff --cached --name-only | Select-String -NotMatch '^(HANDOFF\.md$|docs/|AGENTS\.md$)'
```

- 规则：**服务器 IP / 域名 / Nginx 路径只允许出现在 `HANDOFF.md` 与 `docs/`**。
- ❗ **不要对 `AGENTS.md` 本身跑第 1 条**：它抄了这些正则字面量，会自匹配产生误报（已踩过）。
- 不要用"把 IP 写进正则"的写法，否则规则文件自己就成了泄露源。

---

## 4. 行尾与编码

- 全部 **LF、UTF-8 无 BOM**。已由 `.gitattributes`（`* text=auto eol=lf`）强制，**优先于** `core.autocrlf`。
- 本机 `core.autocrlf` 是**系统级 `true`**（不在仓库里、对协作者不可见），所以别依赖它、也别去改它。
- 验证：`git ls-files --eol` 应全部 `i/lf w/lf`；出现 `w/crlf` / `w/mixed` 说明有文件绕过了规则。
- 落盘方式有坑：PowerShell `Set-Content` / `>` 默认写 BOM，要用
  `[IO.File]::WriteAllText($p,$s,(New-Object Text.UTF8Encoding($false)))`。
- **`.sh` / `.py` 必须 LF** —— CRLF 的 shell 脚本在 Ubuntu 上会 `bash: $'\r': command not found`。
- 想把工作区文件刷成 LF：`git checkout-index -a -f` **不覆盖已存在文件**（实测无效）；
  正确做法是删除后 `git checkout -- <path>`，或 `git rm --cached -r . && git reset --hard`（**会丢未提交改动，先确认干净**）。

---

## 5. C++ 代码风格与分层

- 2 空格缩进、`#pragma once`、私有成员尾 `_`、`Config::` 常量 PascalCase。
- 分层：`main.cpp` 只做装配与 `setup/loop`；逻辑在各 `Controller` / `Sensor` 类里；状态集中在 `SystemState`。
- 一个模块 = 一对 `.h`/`.cpp`，PascalCase 文件名与类名一致。
- **联网功能不得阻塞主循环**；Wi-Fi/服务器不可用时本地 PID、阀门、风扇必须照常运行（`HANDOFF.md` §8 红线）。
- 传感器/执行器/显示逻辑不得因联网功能被改写。

## 6. 结构不变量

```
src/            模块源码（现有 11 个模块 + main.cpp + Config.h）
include/ lib/   PlatformIO 约定目录，目前为空，不要往里塞东西
test/           测试
docs/           设计文档（唯一允许的长期文档存放处）
tmp/            .gitignore 忽略；临时材料、原理图截图、参考 PDF 全放这里；任何二进制都不要入库
platformio.ini  唯一构建配置，env 只有一个：adafruit_feather_esp32s3
HANDOFF.md      交接文档（当前事实）  AGENTS.md  结构约束  docs/ARCHIVE.md  已作废结论
```

新模块放 `src/`，**不要在根目录建新目录**。

## 7. 依赖

当前外部库只有三个：`robtillaart/SHT85`、`bodmer/TFT_eSPI`、`br3ttb/PID`，外加 Arduino 自带 `Wire`/`Preferences`。
加依赖前先确认现有库能否解决；`platformio.ini` 属 🟡（说一句就改），但**新增/升级库要说明理由**。

---

## 8. 自证明细

| 动了什么 | 必须给出的证据 |
|---|---|
| `src/` 改动 | `pio run` 的 **SUCCESS** 原文（含 RAM/Flash 数字） |
| 拆分/重写提交 | 对**每个**中间提交单独检出编译（§2 做法 B），不能只测 HEAD |
| 结构/行尾/索引 | `git status --short`、`git ls-files --eol`、`git diff --stat` |
| 改写历史 | ①备份 + `bundle verify` ②新旧 HEAD `ls-tree -r` 逐文件比对差异为 0 ③提交数差异有解释 ④`git fsck` 无错 |
| 声称已完成 | 给命令与输出，不要凭印象。文档里的数字/结论要有当次实测支撑 |

改写历史后**必须立刻重映射文档里的哈希**：映射表在 `.git/filter-repo/commit-map`（旧 SHA → 新 SHA）。

---

## 9. 会话收尾自查（红旗信号）

每轮结束 harness 会写一条 `agent-summary`（`commands/reads/edits/writes/failed/durationMs`）。出现下面任一项，**主动说明**：

- `reads: 0` 却讨论/引用了文件内容 → **就是编造**（本轮真实事故：模型把没展开的 `@HANDOFF.md` 当作已读，凭空造出一份"R9 修订版"文档，随后 18 分钟都在解释自己编出来的矛盾）
- 单条工具调用耗时 > 5 min → 方法不对，换成限定目录 + timeout
- `failed ≥ 1` → 说清失败在哪、有没有造成损失
- 同一疑问连续 3 次只读探查没结论 → 停，汇报，问

---

## 10. 提交消息风格

与历史保持一致：英文、祈使句、无句号、≤50 字符。真实例子：

```
Replace AHT20 with SHT30 for temperature and humidity
Keep heating switch under user control during sensor faults
Add .gitattributes to pin line endings to LF
Make CO2 polling non-blocking
Persist user settings across restart
```

正文说明"为什么"而不是"改了哪几行"；未验证项标注：`STATUS: work in progress, not yet build-verified`。
