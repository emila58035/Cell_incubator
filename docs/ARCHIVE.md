# ARCHIVE —— 已作废结论与修订历史

> `HANDOFF.md` 只陈述**当前事实**。所有"曾经以为如何、后来发现不对"的考古层集中放在这里，
> 目的是：正文读一遍就能确定当前值，不需要在删除线和批注之间做二次推理。
>
> 本文件由 **R6 文档重构（2026-09-18）** 从 `HANDOFF.md` 正文中迁出，内容一字未改，只换了位置。
> 除追溯决策来路外，接手方不需要读本文件。

---

## 1. 修订历史

| 轮次 | 时间 | 做了什么 |
|---|---|---|
| 汇总 | 2026-09-18 18:35 | DSH 会话汇总前序 Codex 工作，生成本交接文档 |
| R1 | 2026-09-18（Pi harness） | ① 更正 §4.2 的改动归属、文件清单与编译状态（原文误判为来源不明且未验证）② §2.1 补 `platformio.exe` 完整路径 ③ 新增 §4.1.1 记录工作树全量变更 ④ 依据使用者「手机 4G 跳未备案页」反馈与本机 Host 头拦截实测，新增 §5.5 并**重写 §6**（原 DNS-01 路线在备案前无意义，改推 IP 直连）⑤ 新增 §3.3.1 载荷 Schema 缺口、§3.4 并发模型冲突 |
| R2 | 2026-09-18 20:55 | 版本库整备：`tmp/` 取消跟踪、两批改动按主题提交、`.gitattributes` 统一 LF、删除 `backup-master`、仓库转 private |
| R3 | 2026-09-18 21:05 | `HANDOFF.md` 与 `docs/REMOTE_MONITORING.md` 提交入库；因此立刻过时的 5 处陈述同步修正 |
| R4 | 2026-09-18 21:15 | `git filter-repo` 从全部历史移除 `tmp/` 原理图；25→24 提交，`.git` 1.74→0.11 MB；**全文哈希重映射并强推** |
| R5 | 2026-09-18 21:35 | 新增根目录 `AGENTS.md`（结构维护规则 + 禁令清单） |
| R6 | 2026-09-18 22:0x | 文档重构：考古层与作废结论迁入本文件，正文只留当前事实；正文不再写死 commit 哈希 |

### R4 的一条后果值得单独记住

R4 改写历史后，**本文档当时写死的所有 commit 哈希全部失效**，只能靠 `.git/filter-repo/commit-map` 手工重映射。
这就是 R6 之后「正文不写哈希、哈希只出现在 §4.4 / §4.6 这类当次记录表里」这条约定的由来。

---

## 2. 已作废结论对照表

| 原结论（已作废） | 现状 | 出处 |
|---|---|---|
| 批次二（AHT20→SHT30）**未完成且未经构建验证** | 实测 `pio run` SUCCESS（RAM 12.1% / 39548 B，Flash 33.1% / 477781 B）；中间提交单独检出到临时 worktree 构建同样 SUCCESS。「未完成」仍成立，但缺的是**实机验证**与 **PID 复核**，不是编译 | §4.1（原 §4.1.1 接手注意） |
| `HeaterController.cpp` 混装两批改动 → 提交时**要用 `git add -p` 拆开** | 已拆分完成：`SetSampleTime` 常量改名行进批次二提交，加热策略改动进批次一提交（批次一的 diff 已确认不含 `SampleIntervalMs`） | §4.2 |
| 提交时机：等 SHT30 迁移编译通过后用 `git add -p` 拆两个 commit | 已完成，两个提交各自实测可编译 | §4.2 |
| 工作树里有未提交改动，**不要擅自 `git checkout --` 丢弃** | 已无未提交的代码改动 | §4.2 |
| 未跟踪的 `HANDOFF.md`/`docs/` 只有一份本地副本，**切勿 `git clean -fdx`** | 两份文档已提交并推送，有远端副本，`git clean -fdx` 不再造成丢失 | §4.2 |
| `tmp/pdfs/sch1*.png` 仍在历史中，需要 `git filter-repo` + 强推 | R4 已完成，`.git` 1.74 MB → 0.11 MB | §4.4 |
| 原文：「把 DNS 解析迁移到 Cloudflare（或阿里云 DNS）可绕开拦截」 | **不成立**。实测拦截发生在腾讯云网页层、按 Host 头匹配域名，与权威 NS 无关。已删除该建议，改用 §6.1 IP 直连 | §6.2 |
| 原文 §4.2：加热控制改动「来源未确认，可能是本地/IDE 修改」 | R1 已确认由 Pi harness 会话在使用者明确要求下完成，需求原文见 §4.2 | §4.2 |
| 原文：证书签发走 DNS-01 是主线 | 备案解除前 DNS-01 签出的证书对浏览器和 ESP32 都无意义；主线改为 IP 直连 + 自签 CA（§6.1），DNS-01 降为 §6.1.9 暂缓流程 | §6 |

---

## 3. R1 时的 `git status` 全量快照（历史）

当时相对 `master` 的完整差异，分属两批独立工作。**两批现已全部提交**，此清单只作历史留档。

```text
【批次一：加热开关解耦】 Pi harness 会话完成，已编译通过，待实机验证
 M src/HeaterController.cpp      ← 注意：本文件同时含批次二的一行改名，提交须拆分
 M src/HeaterController.h
 M src/InputManager.cpp

【批次二：AHT20 → SHT30 迁移】 使用者本人进行中
 D src/AHT20Sensor.cpp
 D src/AHT20Sensor.h
?? src/SHT30Sensor.cpp           ← 当时未跟踪
?? src/SHT30Sensor.h
 M platformio.ini                lib_deps: dvarrel/AHT20@^1.0.2 → robtillaart/SHT85@^0.6.3
 M src/Config.h                  Aht* 常量 → Sht*（ShtAddress=0x44，SDA=1/SCL=2 不变）
 M src/main.cpp                  #include 与 climateSensor 类型改为 SHT30Sensor
 M src/HeaterController.cpp      SetSampleTime(AhtSampleIntervalMs → ShtSampleIntervalMs)

【文档，当时均未跟踪】
?? HANDOFF.md
?? docs/                         docs/REMOTE_MONITORING.md
```

当时的 R2/R3/R4 批注原文（描述那三批提交的提交链）也已随 R6 重构删除，
对应的权威信息现在在 §4.4 的整备记录表里。

### 加热开关解耦：需求原文（使用者提出，非推测）

1. 开关关 → 不加热
2. 开关开 + 传感器正常 → 加热；传感器故障 → 停止加热**但开关保持 ON**
3. 开关仅由用户输入控制

改动落地的编译验证原始输出：

```text
platformio.exe run → [SUCCESS] Took 6.11 seconds
RAM:   12.1%  (39524 / 327680 bytes)
Flash: 33.1%  (477045 / 1441792 bytes)
```

---

## 4. 被删除分支

`backup-master`：R2 用 `git branch -d` 删除（git 自行校验其已合入 master）。
它曾是 `tmp/` 取消跟踪前 master 的祖先，删除只为清理冗余，无内容损失。
复原需要当次记录表里的哈希（见 `HANDOFF.md` §4.4）。
