# R528 学习终端：当前硬件与链路状态

> 更新时间：2026-09-16  
> 规则：**“源码存在 / 已构建打包 / 已烧录 / 真机通过”是四个不同结论。** 本文只把确有现场证据的项目写为“真机通过”。不会记录 API Key、WiFi 密码或 Authorization 值。

## 总览

| 链路 | 当前结论 | 证据或阻塞 |
|---|---|---|
| MiMo 凭据 | **用户告知 Key 已过期（9/12）** | 本镜像沿用旧配置，没有写入新 Key，也未请求云端核验有效性。需要本地更新有效凭据后再验 ASR、聊天与 TTS；主机测试通过不证明云端可用。 |
| 显示与触摸 | **基础可运行，连续对话后触摸仍需正式复验** | 用户确认上一轮镜像烧录可运行；保留跨任务 TLS 池隔离。不能仅凭“可以了”将所有历史问题标为真机通过。 |
| 原生 UI | **已构建，主要页面可运行** | 首页、状态、任务、AI、设置、开机/待机页共用 `study_terminal_main.c`。 |
| ai_agent 进程 | **popen 栈修复真机初验通过** | 保留 `CONFIG_SYSTEM_POPEN_STACKSIZE=20480`；8/28 日志不再 popen panic。旧 fs_files 并发根因说法已撤销。 |
| WiFi 客户端配网 | **真机通过（手动与自动初验）** | 已有真实 wlan0 IPv4 与联网证据；由 ai_agent netmgr 从本地配置自动连接。 |
| 热点/AP/手机 captive portal | **未实现** | 当前 WAPI AP 与 DHCPD 未启用；不能虚构 AP 命令或把它写成已支持。 |
| 任务/专注/天气/音量 | **源码与镜像已实现，需按项复验** | 通过 `/data/ai_agent/` 文件桥接，见下文。 |
| 本地扬声器 | **真机通过** | `nxplayer` 已播放 1 kHz 测试音。 |
| 开机语音介绍 | **bootvoice2 已烧录有声，清晰度未通过** | 用户确认开机有声；9/5 候选另存衰减 6 dB PCM，原始音频保留，听感待验。 |
| 串口控制台 | **真机恢复正常** | 保留 `nxplayer < /data/audio/bootvoice.cmd &`；用户最新报告串口不卡死。 |
| MiMo TTS | **真机已出声（走 nxplayer 回退）** | 2026-09-15 真机 `voice_test_speak 语音播报测试` → `MiMo TTS playback OK.`，`nxplayer fallback finished: status=0 bytes=84480`，total 5353ms；唤醒监听同时运行再测一次仍 OK（4526ms）。`media_player_open()` 在本板返回 NULL，播放走 `audio_playback.c` 的 nxplayer 回退（PCM 落盘 + `playraw`），**该告警不是故障**。 |
| 数字麦克风 | **硬件/PCM 采集通过** | `hw:snddmic` 可打开，16 kHz / 16-bit / 单声道数据峰值非零。 |
| MiMo ASR | **连续批量识别有现场成功证据** | 用户报告识别较准；日志有多轮 80000 bytes/25 chunks 与成功文本，也有空结果错误。`0 sent` 指未流式发送，不代表 batch ASR 没提交。 |
| 唤醒词 | **匹配逻辑真机通过（2026-09-16 真机复验 9/9 符合预期）** | 2026-09-13 六条用例通过、2026-09-15 复测 `Hello，openvela` / `Hello，OpenVela` / `你好，openvela` 仍 yes，`你好小米` 仍 no——**匹配函数是对的**。但真机发现两个下游缺陷：`voice_wake_test` 只取 `argv[1]`（`Hello, openvela` 被截成 `Hello,` → 误报 no），以及 `mimo_asr.c` 把 ASR 语种写死 `zh`（英文被按中文解码）。两者均已在 2026-09-15 候选修复，**真机复验待烧录**。**仍是云端批量 ASR 轮询，不是离线唤醒**——Key 过期则喊不醒。 |
| 端点门控（静音不上传） | **已构建打包；真机复验待烧录** | 2026-09-16 真机实测：静置 65 s 出现 **16 次 ASR 上传 + 16 次 TLS 握手**（每 4.3 s 一轮），`endpoint gate: silence` **0 次**，而串口打出 `endpoint gate: speech 0ms, uploading` —— 判据 `speech_seen` 对整窗粘滞，被采集起始瞬态（`chunk#1` 峰值 11778–32768）每轮拉起。已改为窗口内累计有声时长（绝对阈值 500 ms，运行时可调 `voice_gate`）。 |
| 对话速度 | **低延迟候选已打包，板端耗时未测** | `mimo-v2.5` 关闭思考、限制输出，去掉云端唤醒应答；等待慢回复期间不重新录音。 |
| 提醒与主页圆环 | **提醒本体用户确认可用；提醒中心与圆环联动待真机验收** | 9/12 候选新增提醒中心（四种内容、10 秒–1 小时、三项快捷、16 条上限、逐条取消、清除全部、5 分钟后再提醒）。主页仍用调度器同一份快照，不建第二套倒计时。83 组主机检查通过。 |
| 离线提示音 | **主机通过，真机待验** | 本地 360 ms 双音（600/800 Hz），不访问 MiMo；到期通知尊重主音量 0，语音页活动期间延后。返回 0 只表示受理，不代表已出声。 |
| 专注记录 | **主机通过，真机待验** | 独立模块 `study_focus_stats.c/.h`；30/60/90 分钟目标、七日统计、连续天数；JSON v2 迁移 v1；未校时轮次进 pending 只合并一次。 |
| 学习工具页视觉 | **主机通过，真机待验** | 9/12 候选把工具页从不透明浅色整屏改成与主页同款（烘焙壁纸 + 深色顶栏/tabbar + 白色半透明卡片）。截图复查见 `tests/renders/`。 |
| 学习报告 | **主机通过，真机待验** | 工具页第三个标签：今日专注 / 完成轮次 / 连续天数 / 近 7 天累计对周目标 / 任务待办。全部由 `focus_stats` 现算，不新增持久化。 |
| `set_llm` | **现场待核对** | 当前源码与诊断 ELF 都有命令，但曾在板端得到 `Unknown command`。 |

## 2026-09-16 端点门控真机复验与修复（当前候选）

用户把板子接回 `COM15`，直接驱动真机复验 09-15 候选。结论：
**09-15 的英文唤醒修复真机生效，但端点门控完全没生效。**
完整记录见 [wake-gate-20260916.md](wake-gate-20260916.md)。

### 一、09-15 修复真机复验：通过

9 条离线用例（`voice_wake_test`，不用麦克风/网络/Key）全部符合预期。
`Hello, openvela` 与 `hello openvela` 从 `no` 翻成 `yes` ——
这是缺陷 A（`voice_wake_test` 只取 `argv[1]`）修复在真机生效的硬证据。

| # | 命令 | 修复前 | 2026-09-16 实测 |
|---|---|---|---|
| 1 | `Hello，openvela` | yes | yes |
| 2 | `Hello, openvela` | no | **yes** |
| 3 | `hello openvela` | no | **yes** |
| 4 | `Hello，OpenVela` | yes | yes |
| 5 | `你好，openvela` | yes | yes |
| 6 | `你好 openvela` | yes | yes |
| 7 | `你好小米` | no | no |
| 8 | `小米同学` | no | no |
| 9 | `你好` | no | no |

### 二、端点门控：完全没生效

`voice_wake_start` 后静置 65 秒，一个字不说：

| 观察点 | 实测 | 期望 |
|---|---|---|
| `batch ASR (with_text)` | **16 次 / 69 s** | 0 次 |
| `vela_tls Handshake OK` | **16 次** | 0 次 |
| `endpoint gate: silence` | **0 次** | 每轮一次 |
| `batch ASR result:`（幻觉文本） | **16 条** | 无 |

铁证是这一行：`[voice] endpoint gate: speech 0ms, uploading` ——
门控读到的累计有声是 **0 ms**，却走了“有语音”分支。
两个读数在同一把互斥锁里取，所以不是竞态而是逻辑：09-15 的门控判的是
`speech_seen`，而它**一旦置位就在整个窗口里黏住**；
采集通路的起始瞬态（`chunk#1` 峰值 11778–32768）每轮都够把它拉起来。

幻觉文本 `你觉得他们大不大？` / `Oh.` / `Why?` / `Yeah.` / `Okay.` 中
**没有一条**匹配唤醒词（`wake phrase matched` = 0），所以从未误唤醒。

### 三、三个假设已用实验排除

| 假设 | 实验 | 结论 |
|---|---|---|
| 唤醒监听把 TTS 饿死 / 打满配额 | 4 分钟静音压测：64 次 ASR 上传 + 65 次 TLS 握手后立刻播报 | **不成立**：TTS 仍成功，`TTS stream failed` = 0 |
| TTS 本身坏了 | 连续重复 `voice_test_speak` 8 次 | **不成立**：**8/8 成功**，3971–5615 ms |
| 对话链路坏了 | `ask 用一句话介绍你自己` | **不成立**：LLM 往返 2605 ms，回答正常 |

→ 用户说的「不能说话」不是 TTS 或对话坏了，而是**设备永远走不到说话那一步**：
唤醒循环一直停在“录音→上传→幻觉文本”里。

### 四、修复

判据换成**窗口内累计有声时长**（`voiced_total_ms`，安静 chunk 不清零），
门控只判 `voiced >= s_gate_min_voiced_ms`（默认 500 ms）。
新增 `voice_gate [ms]` 运行时可调，不用重烧即可校准。

> 第一版还加了“有声占比 ≥ 25%”，被主机测试打回：占空比**依赖窗口长度**
> （夹具 10000 ms vs 板端 2500 ms，同一段 700 ms 语音算出 7% vs 28%），
> 会把真话当噪声丢掉。绝对毫秒与配置无关，故只保留绝对阈值。

### 五、验证状态

- **主机**：`run_checks.py` + `run_ui_checks.py` 合计 **87 PASS / 0 FAIL**
  （voice 27 / chat 3 / cron 23 / api 4 / focus 8 / UI 11+11）。
  新增回归用例 `pcm_pattern == 3`（窗口头部 4 帧有声）：
  补丁前的门控会让它失败（`voice_integration.c:464`），补丁后通过。
- **真机**：本轮改动**已构建打包，尚未烧录**。门控生效与 TTS 出声待烧录后补。

**构建与配对取证**（`verify_pair.py` 全绿）：

| 项 | 值 |
|---|---|
| 审计改动文件 | 5（`unexpected_changes: []`） |
| 已重编改动单元 | 3 |
| ELF/bin 全量配对 | `elf_to_bin_equal: true`，11,859,396 B |
| 镜像 | 40,845,312 B，`8296a1284161445936d0777f0fcd7e1e95866f316a987028340d5733a34fec93` |
| ELF | 68,686,580 B，`10ea9b662de37eddac5d391531e7b0df6d95341d4c9d191cec74d85264f1c277` |
| 载荷校验 | `packed_private_config_matches_seed: true`、`non_model_settings_unchanged: true` |
| 归档 | `img_backups/study-offline-candidate_20260916_145833`，`host_test_groups: 87` |

烧录清单见 [wake-gate-20260916-checklist.md](wake-gate-20260916-checklist.md)。

## 2026-09-15 唤醒词英文识别与唤醒监听流量修复（当前候选）

本轮首次接通板端串口（`COM15` / 1500000 8N1 / CH343），把用户三条反馈
（「英文唤醒词检测不到」「开唤醒词就一直识别语音转文字」「经常不能说话」）
查成**三个互相独立的缺陷**，全部有逐字串口日志支撑。
完整记录见 [wake-gate-20260915.md](wake-gate-20260915.md)。

### 真机实测：匹配逻辑是对的，坏的是命令

`voice_wake_test` 不依赖麦克风、网络或 Key，2026-09-15 在 `vela>` 下逐条实测：

| 用例 | 期望 | 实测 |
|---|---|---|
| `voice_wake_test Hello，openvela` | yes | **yes** |
| `voice_wake_test Hello, openvela` | yes | **no** ← 缺陷 A |
| `voice_wake_test hello openvela` | yes | **no** ← 缺陷 A |
| `voice_wake_test 你好，openvela` | yes | **yes** |
| `voice_wake_test Hello，OpenVela` | yes | **yes** |
| `voice_wake_test 你好小米` | no | **no** |

**缺陷 A（命令层）**：`cmd_voice_wake_test()` 只取 `argv[1]`。NSH 按空格分词，
`Hello, openvela` 被截成 `Hello,`，规范化后只剩 `hello` → 误报 `no`。
用英文键盘输入这句几乎必然踩到，而报告正是引用这条命令当证据。
**已修**：拼接 `argv[1..]` 后再匹配。

**缺陷 B（识别层）**：`mimo_asr.c` 把 `asr_options.language` 写死为 `"zh"`。
官方文档明确「使用 `asr_options.language` 指定语种，**未配置时为自动检测**，
支持取值 `auto`/`zh`/`en`」，且该模型「支持中英双语识别及**自动语种检测**……
自动识别语码混用中的各语言内容」。写死 `zh` 使英文唤醒词被强行按中文解码，
转写文本里不会出现 `helloopenvela` —— 这解释了「中文喊得醒、英文喊不醒」的全部现象。
**已修**：改为 `"auto"`。

### 真机实测：唤醒监听在空转

`voice_wake_start` 后静置 65 秒的串口日志统计：

| 指标 | 实测 |
|---|---|
| 云端 ASR 上传 | **20 次**（上传时间戳平均间隔约 **4.3 秒**） |
| 每次上传体积 | **80,000 字节** PCM |
| TLS 握手 | **21 次**（每轮一次完整握手，无连接复用） |
| 静音时的识别结果 | `诶。` / `1.` / `1.` / `<chinese> Yeah.` / `邪恶猫猫，你觉得男生是？` … |

**缺陷 C（功耗与延迟）**：房间是安静的，设备却每 4.3 秒上传 80 KB 音频并做一次
完整 TLS 握手，7×24 不停（约 840 次/小时、约 2 万次/天），换回来全是幻觉文本。
它持续抢占 WiFi / TLS / CPU，拖慢前台问答与 TTS 播报
——**这就是「经常不能说话」的直接原因**。

**已修**：复用代码里**已有的**本地端点启发式 `track_utterance()`
（判据：均值 ≥ 250 **且** 峰值 ≥ 1200，持续 ≥ 200 ms 才算人声；
原本只用于对话自动断句）。`voice_channel_stop_with_text()` 新增 `require_speech`：

- 唤醒窗口传 `true`：没人说话就**本地丢弃、零网络请求**，返回 `-EAGAIN`；
- 用户主动发起的对话传 `false`：行为与改动前完全一致。

日志改为边沿触发——长时间安静只留一行，检测到人声时才打印一行。

**边界**：这仍是**云端批量 ASR 轮询，不是离线唤醒，也不是流式唤醒**。
门控消掉了空转上传，但没有改变它的本质。真正的解法是本地关键词唤醒（KWS），
R528S3 上需要移植或训练小模型，**按剩余时间（截止 9-20）不现实，不作承诺**。

### 顺带确认：TTS 真机是能出声的

```
[voice] TTS network done: 3318ms
[audio_pb] nxplayer fallback playback: 1760ms
nxplayer> playraw /data/ai_agent/tts_stream.pcm 1 16 24000 0
[audio_pb] nxplayer fallback finished: status=0 bytes=84480
[voice] speak done: total 5353ms (play wait 2035ms)
MiMo TTS playback OK.
```

`media_player_open()` 在本板返回 NULL（媒体服务 RPC 路径不可用），播放走
`audio_playback.c` 的 **nxplayer 回退**：PCM 落到 `/data/ai_agent/tts_stream.pcm`，
再由 NxPlayer `playraw` 播放，84480 字节播放成功。**该告警不是故障**，
此前把 `media_player_open failed` 读成「不能出声」是误判。

用户日志里的 `TTS stream failed: -71`（`EPROTO`）出在 TTS **网络流**上，不是播放。
本轮在唤醒监听同时运行的条件下重测 `voice_test_speak`，仍
`MiMo TTS playback OK.`（total 4526ms），属瞬时网络错误。

### 改动清单

| 文件 | 改动 |
|---|---|
| `packages/ai_agent/src/voice/mimo_asr.c` | `asr_options.language`：`zh` → `auto` |
| `packages/ai_agent/src/voice/voice_channel.h` | `voice_channel_stop_with_text()` 增加 `bool require_speech` |
| `packages/ai_agent/src/voice/voice_channel.c` | 新增本地端点门控，复用 `track_utterance()` |
| `packages/ai_agent/src/voice/voice_wake.c` | 唤醒窗口传 `require_speech=true`；`-EAGAIN` 立即重新开窗 |
| `packages/ai_agent/src/ui/lvgl_ui_channel.c` | 对话路径传 `false`，行为不变 |
| `packages/ai_agent/src/channels/cmd_voice.c` | `voice_wake_test` 拼接全部参数 |
| `contest2026_.../app/hello_app/voice_ui_bridge.c` | **改签名时漏改的调用点**（见下） |
| `tests/voice_integration.c` | 新增 3 条门控用例；唤醒用例补上「有人声」的假麦克风 |

### 验证状态

- **主机**：`tests/run_checks.py` **64 PASS / exit 0**、
  `tests/run_ui_checks.py` **22 PASS / exit 0**，合计 **86 组全过**
  （本轮新增 3 组门控用例）。
- **真机**：本轮改动**已构建打包，尚未烧录**。英文唤醒、门控生效、TTS 出声
  三项的复验数据待烧录后补。**在拿到复验数据前，不把英文唤醒写成「已通过」。**

**构建与配对取证**（`verify_pair.py` 全绿，`exit=0`）：

| 项 | 值 |
|---|---|
| 候选改动文件 | 11 |
| 已重编改动单元 | 7 |
| 本板不编译、显式豁免 | 1（`lvgl_ui_channel.c`） |
| 源码哈希核验 | 202 项全部通过 |
| ELF ↔ BIN | `objcopy -R .note.gnu.build-id` 后逐字节相同 |
| `nuttx.bin` | 11,859,380 B · `b913d2549b5dff48541426326aa9f3ac4f698078be47d2a02320392d01d97b96` |
| `vela_nsh.elf` | 68,685,724 B · `4e9e12b218041a981de075c64b9e92ed57463165b7e5d2f4693103f7fdb06f91` |
| 可烧录镜像 | 40,845,312 B · `b0f7386f80e7f05fe59e9659ba1e7024239e6a203a1ecaf8ee04b6cfc7d5b5f5` |

**`lvgl_ui_channel.c` 的豁免是被断言的**：它属于 `CONFIG_AI_AGENT_LVGL_UI` 路径，
本板未启用（`Makefile` 里那一行还指向不存在的 `src/lvgl_ui/`）。`verify_pair.py`
要求该文件必须**确实不出现**在编译日志里，所以这条豁免不可能掩盖「某文件因别的原因
没编上」；其内容仍由哈希核验。

### 一个会真的出问题的漏改（PTT 路径）

`app/hello_app/voice_ui_bridge.c` **不包含 ai_agent 的头文件**（两边是独立仓库），
自己写了一份 `extern` 原型。给 `voice_channel_stop_with_text()` 加第三个参数时，
该文件被漏改，仍按**两个参数**声明和调用。它**是参与构建的**
（`Makefile` 的 `CSRCS += voice_ui_bridge.c`），所以第三个参数会取寄存器里的
**残留值**——端点门控在 PTT 路径上会随机开或关。唯一可见症状是链接期那条
`-Wlto-type-mismatch`，而它把位置指到了完全无关的 `ccu_nkmp.c`，极易被当成 LTO
噪声划过去（同一日志里 `z_sched_wake` 等确实各有同类噪声）。
**修法**：补上第三个参数，PTT 传 `false`（用户主动按键，不门控）。
**反证**：修好后该警告消失，证明它是真问题。

### 两处同名遗留（不修，如实记录）

- `packages/demos/mini_memo/mini_memo_core.c:796/955` 有同样的两参数声明与调用。
  但 `CONFIG_LVX_USE_DEMO_MINI_MEMO is not set`、构建日志里 0 次出现，**不参与构建**；
  且它不在本轮审计基线范围内，属上游 demo 代码。将来若启用该 demo，需同步补第三个参数。
- `packages/ai_agent/src/ui/lvgl_ui_channel.c` 同样不参与本板构建，已显式豁免（见上）。

### 串口事实更正

此前本文写「串口单行上限 64 字符（`CONFIG_NSH_LINELEN=64`）」，**该说法不适用于
`vela>`**：`vela>` 的 REPL 缓冲区是 `LINE_LEN 256`（`nsh_commands.c:75`），
本轮已实测 40+ 字符含中文命令可直接执行；`CONFIG_NSH_LINELEN=64` 只约束 NSH 下的
`study-terminal.sh`。

## 2026-09-12 唤醒词合规统一（已被 2026-09-15 候选取代）


官方《大赛总览》要求含语音唤醒的项目统一使用「你好，openvela / Hello，openvela」，
本项目此前是「你好小米」，属**合规硬伤**而非体验问题。本轮把 7 处全部改掉，其中
5 处在公共仓 `packages/ai_agent`、2 处在专属仓 `app/hello_app`，按规则要分开走 PR。

同时给匹配加了 **ASCII 大小写折叠**（云端 ASR 返回 `OpenVela` / `OPENVELA` /
`openvela` 都能唤醒），删除品牌唤醒词，保留中性旧别名。模型品牌名「小米 MiMo」
**刻意保留**——规则约束的是唤醒词，不是模型名。

二进制层面已直接验证：内核里 `你好，openvela` 命中 **6** 处（对应 6 个可编译
字符串，第 7 处是头文件注释）、`你好小米` 命中 **0**、`小米 MiMo` 命中 1。
测试里也固化了「「你好小米」必须唤不醒」这条断言。

完成 **83 组**主机检查、官方 make 构建、Dragon 打包、ELF/bin 全量配对、YAFFS
载荷核验和 Windows/VM 双端 SHA256 核验。**用户已于 2026-09-13 烧录到真机**，
测试清单见 [flash-checklist-wake-word-20260913.md](flash-checklist-wake-word-20260913.md)。
**烧录 ≠ 真机通过**：目前尚无逐项回报，唤醒词端到端与 AI 问答因 Key 过期仍不可验。

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_wake-word_20260912_256Mnand.img` |
| 镜像大小 / SHA-256 | 40,841,216 bytes / `e7ce47c3983bbe652dba431c52e1a419eafd9f5ec46c1c540618d093f42a52ea` |
| 配对 ELF | `archive/nuttx-20260912-wake-word-candidate.elf` |
| ELF 大小 / SHA-256 | 68,678,904 bytes / `4e08ff20e6f4efb690547b7c0f025314011a13164e645e1d911d172de0478881` |
| 内核 bin 大小 / SHA-256 | 11,855,284 bytes / `a0762889cf987aa3e22227bf5e75dacfbb83c2cb1e13246b0ad0a14ed73d6b0a` |
| 镜像生成时间（UTC） | 2026-09-12T16:00:57.213143+00:00 |
| VM 配对快照 | `/home/openvela/img_backups/study-offline-candidate_20260912_160113` |
| 状态 | **已构建打包、已配对备份；用户已烧录（2026-09-13），唤醒词六条用例真机全部通过；端到端与 AI 问答待有效 Key。** |

完整记录：[wake-word-20260912.md](wake-word-20260912.md)。私有配置本轮逐字节未变，
聊天模型仍是 `mimo-v2.5`；镜像含预置凭据（含已过期 Key），不可外发。

### 唤醒词真机验证：已全部通过（2026-09-13）

用户在真机上执行了 `voice_wake_test`（该命令直接调用 `voice_wake_matches()`，
**不依赖麦克风、网络或任何 Key**），**六条用例全部符合预期**：

| 用例 | 期望 | 实测 |
|---|---|---|
| `voice_wake_test 你好，openvela` | yes | **yes** |
| `voice_wake_test 你好 openvela` | yes | **yes** |
| `voice_wake_test Hello，OpenVela` | yes | **yes** |
| `voice_wake_test 你好小米` | no | **no** |
| `voice_wake_test 小米同学` | no | **no** |
| `voice_wake_test 你好` | no | **no** |

→ **唤醒词短语匹配与品牌词隔离已在真机上验证通过**，这是本轮合规改动的硬证据：
新唤醒词能命中、旧品牌唤醒词「你好小米」及其近似词「小米同学」都**唤不醒**，
且纯「你好」不会误触发。ASCII 大小写折叠（`OpenVela` / `OPENVELA`）也已实测生效。

**边界仍要说清**：这只证明**匹配逻辑**正确。真正的**端到端唤醒**（对着麦克风说
→ 云端 ASR 转写 → 匹配 → 自动开页）仍需有效 ASR Key，目前**不可验**。

**关于系统时钟的重要澄清**：串口日志出现
`[vela_tls] System clock not set (UNIX=1822); NTP has not synced`，
但**这不是 TLS 失败的原因**。`vela_tls.c:313-331` 明确注释：

> Nothing here needs a valid clock: authmode is VERIFY_OPTIONAL, so
> certificate validity windows do not gate this handshake.

历史上"开机 `clock_settime()` 到硬编码 2026-02-28"的做法已**故意移除**
（它会掩盖 `ntpc` 失败、让 UI 头显示假时间、且让所有调试会话时间戳雷同）。
**排查联网失败时不要走时钟这条线**，根因是凭据。

**能测（不需要模型）**：五页 UI、触摸、中文显示、系统状态、全部离线学习工具
（提醒中心 / 离线提示音 / 五分钟稍后提醒 / 专注目标 / 七日记录 / 学习报告），
以及上面的 `voice_wake_test` 短语匹配。

**不能测**：语音唤醒**端到端**（需有效 ASR Key）、AI 问答。

**Key 不用重烧就能换**：`router_set mimo <key>` 或 `router_set <preset> <key>`
（支持 deepseek / kimi / qwen / openai 等），之后 `router_status` 确认。

**注意**：`voice_wake_test` 属 `vela>` 提示符下的命令（`nsh>` 下先执行 `ai_agent`）。
**2026-09-15 更正**：此前写的「串口单行上限 64 字符（`CONFIG_NSH_LINELEN=64`）」不适用于
`vela>`——`vela>` 的 REPL 缓冲区是 `LINE_LEN 256`（`nsh_commands.c:75`），本轮已实测
40+ 字符含中文命令可正常执行；`CONFIG_NSH_LINELEN=64` 只约束 NSH 下的 `study-terminal.sh`。
测试时请在干净的 `vela>` 提示符下逐条执行——在 `recording...` 期间输入会与提示符输出交错
（首次日志即如此，虽然结果仍有效，但后续复测建议避开）。

### 遗留风险

`openvela` 是英文词，中文语境下云端 ASR 可能转写成别的形式，而 MiMo Key 已过期，
无法实测识别效果。真机若唤不醒，补救方式是在短语表里补上 ASR 实际返回的变体——
短语表就是为这种情况设计的，不需要改逻辑。

### 基线前移的时序问题（本轮发现）

本轮审计一开始报 `unexpected: study_terminal_main.c`。根因是**上一轮的基线前移
跑在了同步之前**，导致基线落后树一轮。已确认树的哈希正是归档轮次
`study-offline-candidate_20260912_153518` 的副本（基线里是
`study-offline-candidate_20260912_151027` 的副本），前移后与归档逐字节匹配。
**后续每轮的固定顺序是：前移基线 → 审计 → 同步。**

## 2026-09-12 学习工具页视觉统一 + 学习报告（历史）

在离线学习工具候选之上，把新功能界面拉回主页同一套视觉语言（烘焙壁纸 + 深色玻璃壳
+ 白色半透明卡片），并新增第三个标签「学习报告」和「清除全部提醒」。
公共仓库 `nuttx/`、`packages/ai_agent/` 本轮**零改动**，唯一源码改动是
`contest2026_120_haohaoxuexitiantianxiangshang/app/hello_app/study_terminal_main.c`。

完成 **83 组**主机检查（voice 23、chat 3、cron 23、focus 8、api 4、UI 两种尺寸各 11）、
官方 make 构建、Dragon 打包、ELF/bin 全量配对、YAFFS 载荷核验和 Windows/VM 双端
SHA256 核验。**未烧录，不能宣称学习工具页或学习报告已真机通过。**

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_study-ui-consistency_20260912_256Mnand.img` |
| 镜像大小 / SHA-256 | 40,841,216 bytes / `e2b97d1aedf564e85b044111276f0515ef52ba3997b9846686f2a167ccf2a85a` |
| 配对 ELF | `archive/nuttx-20260912-ui-consistency-candidate.elf` |
| ELF 大小 / SHA-256 | 68,678,880 bytes / `b20bba64006cd7d2ae3fbcb31f619c8f3583c55d16a40dcf1c12fd02d2422ae8` |
| 内核 bin 大小 / SHA-256 | 11,855,284 bytes / `539617c6a43d15f27388381f5877e4772b744f15d8c033fb46b8b90255a5029a` |
| 镜像生成时间（UTC） | 2026-09-12T15:35:03.046454+00:00 |
| VM 配对快照 | `/home/openvela/img_backups/study-offline-candidate_20260912_153518` |
| 状态 | **已构建打包、已配对备份；未烧录、真机待验。** |

完整记录：[ui-consistency-20260912.md](ui-consistency-20260912.md)。私有配置本轮
逐字节未变，聊天模型仍是 `mimo-v2.5`；镜像含预置凭据（含已过期 Key），不可外发。

### 学习报告的取值边界

报告页所有数字都由 `focus_stats` 现算，不缓存、不新增持久化，因此和七日柱状图、
主页计数永远同源，不会互相矛盾。任务待办数只在报告页可见时读取 `STUDY_TASKS.md`，
不额外增加每秒文件 IO。日期未同步时显示"日期未同步，今日数据待校时"，
不把待校时数据当成已确认。

### 基线审计语义修正（本轮）

`audit_candidate.py` 原先把"候选目录里树中不存在的路径"也算作漂移，第三轮新增的
`study_focus_stats.c/.h` 在同步后必然误报。现已拆成 `changed`（与当前树不同，需同步）、
`added`（不在继承基线，本线新增）、`unexpected`（树与基线不一致）三个独立结论。
基线本身是一次性的：第三轮同步后树已合法偏离继承基线，新增 `rebaseline_candidate.py`
前移，且要求每个漂移文件必须与某个已归档候选轮次的副本逐字节相同，否则拒绝前移。
本轮 12 个漂移文件全部可归因，确认无第三方改动。

## 2026-09-12 离线学习工具候选（历史）

新增提醒中心、离线提示音和专注记录，全部**不依赖 MiMo 云端**。MiMo Key 已过期，
本轮没有请求云端，也没有把鉴权失败当作模型慢。

完成 81 组主机检查（voice 23、chat 3、cron 23、focus 8、api 4、UI 两种尺寸各 10）、
官方 make 构建、Dragon 打包、ELF/bin 全量配对、YAFFS 载荷核验和 Windows/VM 双端
SHA256 核验。**未烧录，不能宣称提醒中心、离线提示音或专注记录已真机通过。**

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_study-offline_20260912_256Mnand.img` |
| 镜像大小 / SHA-256 | 40,841,216 bytes / `74f98365e632b6d240a848738bab33234439b71f793cee0689760a3f9f3e1c33` |
| 配对 ELF | `archive/nuttx-20260912-study-offline-candidate.elf` |
| ELF 大小 / SHA-256 | 68,670,248 bytes / `4928276d4d7d80c999858927113544b0d36cb366143f04d5cb7ae5c472c6c5e0` |
| 内核 bin 大小 / SHA-256 | 11,855,284 bytes / `5510c0e2e9cf42501f1699fdb2125ab46c2a938198e3c0e2361c0b9aa821f645` |
| 镜像生成时间（UTC） | 2026-09-12T15:10:10.193099+00:00 |
| VM 配对快照 | `/home/openvela/img_backups/study-offline-candidate_20260912_151027` |
| 状态 | **已构建打包、已配对备份；未烧录、真机待验。** |

完整记录：[study-offline-20260912.md](study-offline-20260912.md)。私有配置本轮
逐字节未变，聊天模型仍是 `mimo-v2.5`；镜像含预置凭据（含已过期 Key），不可外发。

### 提醒中心的断电恢复边界

有可信保存日期的未来提醒在校时后恢复。未校时期间创建、又没有可信日期的倒计时
**无法推断断电时间**，重启时清除，不假装准确恢复。失效的一次性提醒做容量回收，
避免列表空白却占满 16 个槽位。

## 2026-09-11 连续语音对话候选（历史）

已接续未完成的语音/界面候选，完成 24 个文件的同步、38 组主机检查、ARM make、官方打包、ELF 全量配对及 Windows/VM 双端哈希核验。新候选**未烧录，不能宣称连续语音、触摸或回复速度已真机通过**。

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_voice-dialogue-prekeyed-candidate_20260911_256Mnand.img` |
| 镜像大小 / SHA-256 | 40,824,832 bytes / `af1655aee2f72607a67cb37601dfa74a5717dd5c0f458a0a7b39d3d9bfce54ce` |
| 配对 ELF | `archive/nuttx-20260911-voice-dialogue-candidate.elf` |
| ELF 大小 / SHA-256 | 68,584,104 bytes / `9bb192d148da9ba5cb36e5f40a3b193d0b7e3c360f5484256f9df848e8ce6a11` |
| 内核 bin 大小 / SHA-256 | 11,838,852 bytes / `a2b4f5583450643da5cfbfa577b9968ba0045efade98f15f98328b14f33347d3` |
| 镜像生成时间（UTC） | 2026-09-11T15:51:29.764816+00:00 |
| VM 配对快照 | `/home/openvela/img_backups/voice-dialogue-candidate_20260911_155223` |
| 状态 | **已构建打包、已配对备份；未烧录、真机待验。** |

完整改动、边界和验收步骤：[voice-dialogue-20260911.md](voice-dialogue-20260911.md)。预置配置只切换模型，原凭据及其他字段已校验保留；镜像禁止外发。

## 2026-09-06 复验失败与历史修复

本节保留当时的交付记录。用户后来已确认上一轮镜像烧录可运行，但对话回复与界面联动仍有问题；当前应使用上方 9/11 候选验收。

用户已烧录 9/5 候选，报告 AI 回复后触摸仍失效、不能继续对话、回复不播报、唤醒词不可用。该版不能算修复通过。开机介绍新音频的听感尚无新反馈。

本次找到两个可复现的代码缺陷：全局 HTTPS 连接池跨 NuttX 任务复用 fd，可能误读或误关另一个任务的字体/触摸文件；唤醒录音与前台 PTT/TTS 没有协调。旧源码分别复现了误关文件和 TTS 返回 `-EBUSY`，新源码已通过对应主机测试和真实语音模块联测。新候选已完成 make、官方打包、ELF/bin 全量配对和 Windows/VM 哈希校验，尚未烧录。[本次记录](voice-reply-20260906.md)。

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_voice-replyfix-prekeyed-candidate_20260906_256Mnand.img` |
| 镜像大小 / SHA-256 | 40,816,640 bytes / `541cec4fe763eaca0fdce9d9882c2db33ebc566f9f0b72ba28bf5c16a3a490b6` |
| 配对 ELF | `archive/nuttx-20260906-voice-replyfix-candidate.elf` |
| ELF 大小 / SHA-256 | 68,547,832 bytes / `aa91ddf65eb70cd7500ce04db25449964ba7386594f3151235428b167bce8f60` |
| 内核 bin 大小 / SHA-256 | 11,830,628 bytes / `65009226fa2f41ca30a8b6051201fd9ef798e15c6b030a0308c3e2e55f034a28` |
| 镜像生成时间（UTC） | 2026-09-06T05:37:48.007897+00:00 |
| VM 配对快照 | `/home/openvela/img_backups/voice-replyfix-candidate-20260906_20260906_053900` |
| 状态 | **已构建打包、已配对备份；未烧录、真机待验。** |

## 当前产品功能

### 数据文件与服务边界

| 功能 | 数据/服务 | 当前行为 |
|---|---|---|
| 任务 | `/data/ai_agent/STUDY_TASKS.md` | 首页显示真实待办数和首项待办；任务页读取较长列表、支持滚动和清理已完成项；仅在内容变化时重建，避免吞掉触摸。 |
| 专注 | `/data/ai_agent/STUDY_FOCUS.json` | 15、25、45 分钟可选，5 分钟休息，保存累计专注数据。 |
| 提醒 | cron 普通值快照 | 主页优先显示最近本地提醒的剩余时间、进度和取消按钮；不覆盖专注统计。 |
| 音量 | `/data/ai_agent/STUDY_VOLUME.json` | 滑块保存 0–100；已将“值变更持久化”和“松手预听”分离，防止松手写回旧值。待真机确认。 |
| 天气 | `weather_service.c` → `/data/ai_agent/WEATHER.json` | 高德 HTTPS 拉取并原子写文件；未设城市时默认深圳；刷新默认 1800 秒、最小 300 秒。 |
| AI 页面 | UI bridge / 最近回复文件 | 显示 agent 运行、忙碌、语音状态和最近最终回复，而不是假状态。 |
| 设置 | agent config JSON | 城市点选会读改写 `weather.city`，保留其中的其他配置键。 |

天气城市的板端点选列表：深圳、广州、北京、上海、杭州、成都、武汉、西安。需要其他城市时，仍可用 CLI 指定中文城市名或行政区划代码。

### 配置命令与安全边界

下列命令属于 `vela>` 提示符；不要在文档或串口日志中粘贴真实密钥：

```text
config_show
set_llm mimo <MIMO_API_KEY>
set_weather <AMAP_KEY> 深圳
```

- 高德 Key 应为 **Web 服务** Key，不是 JS/Web API Key。
- MiMo 聊天、TTS 与 ASR 设计上共享同一 host/API key 快照；这避免每个语音子模块各自保存一份密钥。
- 已发生过真实密钥进入交互记录的情况；应在服务商控制台轮换受影响密钥。

### `set_llm` 的现场不一致

源代码 `packages/ai_agent/src/channels/nsh_commands.c` 的 help 和 dispatch 都有 `set_llm`；9/11 候选的 `cmd_llm.c` 中，`mimo` 预设指向 Token Plan 主机与 `mimo-v2.5`。最新配对 ELF 同样包含命令。

但曾有板端输入 `set_llm mimo <key>` 后返回 `Unknown command: set_llm`。当前有效的 `LINE_LEN` 是 256，而 NuttX `CONFIG_STDIO_BUFFER_SIZE=64`、`CONFIG_NSH_LINELEN=64`；它们值得检查，但**尚未证明**是根因。优先在烧录后运行 `help` 与 `config_show`，核对实际镜像和进程，而不是直接修改多个缓冲区参数。

## 已确认的底层结论

### G2D/DMA 堆破坏：已修复且已解释

根因不是 LVGL 图标、字形、IRQ 栈或 sem 竞态：G2D fill ioctl 把 RGB565 framebuffer 的目标格式硬编码成 `0`（ARGB8888 语义）。于是按每像素 4 字节写入实际每像素 2 字节的显存，写过 framebuffer 尾端并破坏后面的堆对象。这个问题能解释随机崩溃、异常点远离写入点、以及 framebuffer 像素特征。

对应修复必须持续保留。`sem_post.c` 等崩点只是受害者位置，不是根因。

### 启动脚本：64 字符规则已修复

曾出现：

```text
Failed to initialize module image: -2
nsh: boot: boardctl failed: 1
```

根因是 `CONFIG_NSH_LINELEN=64` 截断了启动脚本中的长**注释**，余下的 `boot` 变成独立命令，脚本提前中止。当前 `study-terminal.sh` 所有行（含注释）必须少于 64 字符。

当前实际的启动顺序为（与打包脚本逐行一致，见 `docs/study-terminal.sh`）：

```text
study_terminal &
mediad &
sleep 5
nxplayer < /data/audio/bootvoice.cmd &
ntpcstart
ai_agent --no-cli &
```

UI 先起以缩短黑屏等待；当前 `mediad` 因 `amovie_async` 与 FFmpeg 注册表不匹配，在打开音频设备前失败，因此不能声称它已占用 codec。nxplayer stdin 回退保留；`ntpcstart` 在 agent 前启动。

### nxplayer 忽略 argv：一个根因造成“没声音”和“串口不能用”

`apps/system/nxplayer/nxplayer_main.c:790` 的 `int main(int argc, FAR char *argv[])` **通篇没有引用过 argc 或 argv**。它打印版本横幅后直接进 REPL：

```c
while (running)
  {
    printf("nxplayer> ");
    len = readline_stream(buffer, LINE_MAX, stdin, stdout);
    ...
  }
```

因此 `nxplayer <path> &` 有两个后果：

1. 路径参数被静默丢弃，**永远不会播放任何东西**。
2. 它挂在 stdin 上；在串口上后台运行时就和 NSH 抢同一个字符流。用户日志里 `play nxplayer> /data/audio/boot_intro.wa` 就是两个读者各抢到一半——`nxplayer> ` 提示符插进用户输入中间，路径被截断成 `.wa`。

`CONFIG_NXPLAYER_COMMAND_LINE=y` 是误导项：它的 Kconfig 帮助只说“compiles in code for the nxplayer command line control”，指的就是这个 REPL，**不是** argv 支持。

正确用法是喂 stdin。已逐环节读源码确认的执行链：

| 环节 | 源码位置 | 结论 |
|---|---|---|
| `<` 解析 | `apps/nshlib/nsh_parse.c:272,2784-2806` | 取出文件名存入 `param->file_in`，`oflags_in = O_RDONLY`，并从 argv 中移除 |
| `&` 解析 | `apps/nshlib/nsh_parse.c` 尾部 | 置 `np_bg = true` 并 `argc--` |
| 分发 | `nsh_parse.c:557` `nsh_execute()` | `CONFIG_NSH_BUILTIN_APPS=y` 且 `BUILTIN_AS_COMMAND` 未设，所以**先**调 `nsh_fileapp(...param)`，绕过后面的 `sh -c` 后台包装 |
| 重定向落地 | `apps/nshlib/nsh_fileapps.c:157` | `posix_spawn_file_actions_addopen(&file_actions, 0, param->file_in, ...)`，即子进程 fd 0 = 命令文件 |
| 后台不阻塞 | `nsh_fileapps.c:361` | `if (vtbl->np.np_bg == false)` 才 `waitpid`；有 `&` 就不等，脚本继续往下跑 |

即 `nxplayer < /data/audio/bootvoice.cmd &` 会让 nxplayer 从文件而不是控制台读命令，串口 stdin 仍归 NSH，两个症状一起消失。

`mediatool` 不是替代方案：`apps/frameworks/multimedia/media/media_tool.c` 的 `main` 同样 `(void)argc; (void)argv;`，并且在 `getline` 返回 −1（EOF）时 `continue` 而不是退出，缺陷完全一样。


### 时钟与 TLS：旧“成功”结论已撤销

- 板子没有 RTC，冷启动时间为 1970 是预期初始状态。
- VM 的 NTP/HTTP Date 交叉验证正常；不是 VM 时间源错误。
- `vela_tls.c` 曾在检测到旧时钟时强制写入 2026-02-28。该伪造值让 UI 误以为时钟同步，且污染所有日志时间；现在已删除，只保留“未同步”的 warning。
- TLS 认证模式是 `MBEDTLS_SSL_VERIFY_OPTIONAL`，因此不应再声称旧时钟会使当前握手“必然 fail closed”。
- mbedTLS 的 ASN.1、PK、PK parse、RSA、X.509 CRT parse 已实际启用，最终 ELF 也含 RSA/PK parser 符号。此前“固件缺 RSA parser”的说法不正确。
- TLS 连接池失效重连路径已调用 `tls_ctx_free()`；“旧 pooled context 未释放”也不是已证实根因。

### MiMo TLS `-0x3b62`：等待精确诊断

错误组合为 `PK_INVALID_PUBKEY + ASN1_UNEXPECTED_TAG`。MiMo 的链条在主机侧可按正常 RSA-2048 解析，因此新增诊断在 `x509_crt.c` 的 `mbedtls_pk_parse_subpubkey()` 失败点直接打印 SPKI 开头字节。

有效 RSA SPKI 预期近似：

```text
30 82 .. .. 30 0d 06 09 2a 86 48 86 f7 0d 01 01 01
```

- 若没有 `30` SEQUENCE 起始，优先按内存/缓冲被覆盖调查。
- 若字节正确，调查 mbedTLS 解析或链路行为。

在抓到 `[x509diag]` 输出前，不把任一方向写成已定论。

## 音频与语音

### 已通过的部分

- `hw:snddmic` 直接 ALSA 采集可打开，日志确认请求 16 kHz、单声道、16-bit，且首块 3200 bytes 的峰值非零。
- `nxplayer` 本地 1 kHz 测试音曾通过物理扬声器。
- 历史“direct MiMo TTS audible-wait”镜像曾完成请求、解码、直通 `nxplayer` 回退并听到“你好”。这不能替代当前 TLS 诊断结果。

### 历史采集阻塞与当前待验

2026-09-05 源码与配对 ELF 复核：避免重复 `snd_vela_pcm_prepare` 的修复已经进入 bootvoice2 内核。下面是 8/28 的历史失败证据，不代表该修复尚未实现。后续仍需验证连续多秒采集与重复 ASR；本次候选另修异步 UI、错误返回和 PCM 所有权。

**2026-08-28 真机复测（popenstack 镜像）**：麦克风仍无法持续采集。新证据：直接 ALSA 打开成功、第一块 PCM 有真实音量（peak 21750~29652），但录音线程循环中的第二次 `snd_vela_pcm_prepare` 失败（`ALSA prepare failed`），线程只读到 1 块就退出（`1 chunks, 3200 read, 0 sent`）；ASR 流式回调为空、走 batch fallback 但 `0 sent`。该历史问题的状态检查修复已在当前基线，等待连续采集复验。

现场日志表明：

```text
[voice_asr] Active ASR backend has no streaming support
[voice] streaming ASR unavailable, batch fallback
[audio_cap] ALSA prepare failed
[voice] recording thread exit: 1 chunks, 3200 read, 0 sent
```

这说明：

1. `mimo_asr.c` 的 `stream_open`、`stream_send`、`stream_finish` 都是 `NULL`，因此不能走流式 ASR。
2. 虽然 PCM 首块已采到，但后续 ALSA prepare/read 失败，录音线程提前退出。
3. `0 sent` 表示音频从未提交到 ASR，不是“识别结果不准”。
4. `media_player_open failed` 后的 `nxplayer` 回退与 `mediad` 启动故障应独立排查，不能把它混同为麦克风损坏。

验收顺序：先复验直接 ALSA 持续采集，再复验 batch fallback 和重复对话；流式回调与唤醒词尚未验收。

## 历史配对构建产物

### 语音/触摸候选（2026-09-05，未烧录）

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_voice-touch-prekeyed-candidate_20260905_256Mnand.img` |
| 镜像大小 / SHA-256 | 40,816,640 bytes / `f73a1e8210d663d1108cd122000116e55e06bb8ae0b9647aa8a3864450da1df2` |
| 配对 ELF | `archive/nuttx-20260905-voice-touch-candidate.elf` |
| ELF 大小 / SHA-256 | 68,539,352 bytes / `cb2dc6fccf54677aca49e40337abcb3721c498c534006b55cfbb0d060612d503` |
| 内核 bin 大小 / SHA-256 | 11,830,596 bytes / `f93b7956372b8dd0b71afc4eb43122dade2968451ee4cd0496e5fe55fbc33844` |
| 镜像生成时间 | 2026-09-05T15:23:27.111405+00:00（UTC；北京时间 2026-09-05 23:23） |
| VM 成对快照 | `/home/openvela/img_backups/voice-touch-candidate-20260905_20260905_155533` |
| 状态 | **已构建打包、已成对备份；未烧录，真机复验待完成。** |

[改动、验证与真机待验](voice-touch-20260905.md)。镜像含用户授权预置凭据，不可外发。

### 开机语音修复版 bootvoice2（2026-09-04）

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_ui-dailyplan-coach-prekeyed-wifi-popenstack-bootvoice2_20260904_256Mnand.img` |
| 镜像大小 | 40,206,336 bytes |
| 镜像 SHA-256 | `3443b768dc15527e539fe1a737de9435639abb2858924857534c16734887d7fe` |
| 匹配 ELF | `archive/nuttx-20260904-bootvoice2-verified-pair.elf` |
| ELF SHA-256 | `fa0593cd6eb694426dd8dc0aa8fea7e9142d96c7f91a68bb96ef0bf6f47e9ba6` |
| 内核 | **未变**，`nsh.fex` = `nuttx.bin` SHA `fd417f2fcaa85359ac04233e5f188062e36cbe69ae09606b073b20e471aa48a6`，与 popenstack 版同一内核；本次只重打包 usrdata。 |
| 内容 | 修复开机语音与串口被抢两个症状（同一个根因）。新增 `/data/audio/bootvoice.cmd` 与去头 PCM `/data/audio/boot_intro.pcm`（599,040 bytes，从 WAV 的 data chunk 偏移 44 提取）；启动脚本改为 `nxplayer < /data/audio/bootvoice.cmd &`。 |
| 打包证据 | Dragon `execute image.cfg SUCCESS !` + `pack finish`；yaffs 日志逐个列出 `bootvoice.cmd`（1 chunk）、`boot_intro.pcm`（295 chunks）。`PACK_RC=1` 是已知的外层脚本假失败。 |
| 离线验证 | 在最终 `.img` 里：新行 `nxplayer < /data/audio/bootvoice.cmd &` 命中 1 次；旧行 `nxplayer /data/audio/boot_intro.wav &` 命中 **0** 次；`playraw /data/audio/boot_intro.pcm 1 16 24000 0` 与 `!sleep 14` 各命中 1 次。 |
| 烧录状态 | **已烧录；用户确认开机有声、串口正常。** 声音模糊和语音后触摸失效仍存在。 |

以下为已验证 bootvoice2 的历史命令；9/5 候选只将 PCM 路径改为 `boot_intro_soft.pcm`，保留四行结构：

```text
volume 100
playraw /data/audio/boot_intro.pcm 1 16 24000 0
!sleep 14
q
```

- `volume 100` → `nxplayer_cmd_volume` 做 `atof(parg) * 10.0`，即内部 1000（满音量）。
- `playraw <file> <ch> <bits> <rate> <chmap>` 是 `packages/ai_agent/src/voice/audio_playback.c` 现场出过声的同一条命令，绕开 nxplayer 自己解析 WAV 头。
- `!sleep 14` 必需：`nxplayer_playraw` 的注释写明 `OK = File is being played`，是**异步**的，紧跟 `q` 会立刻 `nxplayer_stop` 掐掉播放。音频 12.48s，14s 留余量。`!` 分支走 `system()`，`CONFIG_SYSTEM_SYSTEM=y`、栈 20480，`sleep` 是 NSH builtin（argc 2..2）。
- `q` 必需：`nxplayer_main.c` 的主循环在 `len <= 0`（EOF）时**没有 break**，会无限打印 `nxplayer> ` 空转。

### 开机语音介绍版（2026-08-28）——**已被 bootvoice2 取代，不要再烧**

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_ui-dailyplan-coach-prekeyed-wifi-popenstack-bootvoice_20260828_256Mnand.img` |
| 镜像 SHA-256 | `0b7352d9a1711bfe9d6802578d72e4eab672431bd9eaaa84fed0228e55348952` |
| 内容 | 用 MiMo `mimo-v2.5-tts` 生成开机中文介绍 WAV（12.48s/24kHz/mono/16bit），打包到 `/data/audio/boot_intro.wav`，启动脚本写 `nxplayer /data/audio/boot_intro.wav &`。 |
| 实际结果 | **已烧录，功能为零且造成回归。** 开机无声（路径被丢弃），并且串口控制台不可用（nxplayer 抢 stdin）。 |
| 当时的验证 | 只 grep 了 `usrdata.fex` 里有没有那行字符串。字符串在，功能无。**这是本项目最典型的一次无效验证。** |


### popen 栈溢出修复版（2026-08-28）

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_ui-dailyplan-coach-prekeyed-wifi-popenstack_20260828_256Mnand.img` |
| 镜像 SHA-256 | `4c26bce01f85d14152345cb9723fd4e2e3a573263a2db1626213f1dc5322ec2d` |
| 匹配 ELF | `archive/nuttx-20260828-ui-dailyplan-coach-prekeyed-wifi-popenstack.elf` |
| ELF SHA-256 | `44bdeb56a28057c585197e5d7cd1b35f14ce6001bd5c58f82b8051c6c48f434a` |
| 内容 | 全量重建内核，`CONFIG_SYSTEM_POPEN_STACKSIZE` 从 2048 提升到 20480，消除 ai_agent `popen("wapi status wlan0")` 引发的 NSH 栈溢出写坏堆；推翻阶段 15 的 fs_files 并发假设。 |
| 烧录状态 | **已烧录初验（2026-08-28）**：不再 popen panic，WiFi 自动连并联网；麦克风持续采集仍失败。 |

### UI 今日计划/智能建议版（2026-08-26）

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_ui-dailyplan-coach_20260826_256Mnand.img` |
| 镜像 SHA-256 | `20390d384294c29c7e6e722b666ac6ff3e4416845d030964f42b81e3027745db` |
| 匹配 ELF | `archive/nuttx-20260826-ui-dailyplan-coach.elf` |
| ELF SHA-256 | `5b57f69f8c2683a7c7a42caff677678d92290cdc7017f040e2293d747940f112` |
| 内容 | 合入 8/18 的“今日计划/智能建议/演示就绪度”UI 增强；TLS/ASR 相关代码与 8/15 诊断版一致。 |
| 烧录状态 | **未烧录、未真机验证。** |

### UI 预置凭据版（2026-08-26）

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_ui-dailyplan-coach-prekeyed_20260826_256Mnand.img` |
| 镜像 SHA-256 | `71b0ca08b2acff5b48f64e3e8cc443936acea3b8fafb861c83430c399c8b57d3` |
| 匹配 ELF | `archive/nuttx-20260826-ui-dailyplan-coach.elf`（内核未变） |
| ELF SHA-256 | `5b57f69f8c2683a7c7a42caff677678d92290cdc7017f040e2293d747940f112` |
| 内容 | 与 UI 版相同，另按用户要求把天气/MiMo/WiFi 凭据预置进 `/data` 种子。**镜像含明文密钥，严禁外发。** |
| 烧录状态 | **未烧录。** 烧录后可直接测试天气、MiMo、WiFi。 |



### 启动错峰修复版（2026-08-27 第二次）
| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_ui-dailyplan-coach-prekeyed-wifi-agentfix_20260827_256Mnand.img` |
| 镜像 SHA-256 | `d592dd647cf770270a4a83c0582f4e63df6d9543ed449fe01a0a3a0d95c48d04` |
| 配对 ELF | `archive/nuttx-20260826-ui-dailyplan-coach.elf`（内核未变） |
| 内容 | 在 wifi 版基础上把启动改为错峰（mediad 后 sleep 5 再起 ntpc/ai_agent），缓解启动期 fd 冲突；修复对象是“AI 服务未运行”。 |
| 烧录状态 | **未烧录。** 若仍失败需抓串口日志（P0-P5 标记）。 |

### UI 预置凭据 + WiFi 自动连接版（2026-08-27）
| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_ui-dailyplan-coach-prekeyed-wifi_20260827_256Mnand.img` |
| 镜像 SHA-256 | `d5fdf37ff66d90e056ff19df403e2b9b08fae2a2aa90e6c64b204f13d041dbb9` |
| 配对 ELF | `archive/nuttx-20260826-ui-dailyplan-coach.elf`（内核未变） |
| ELF SHA-256 | `5b57f69f8c2683a7c7a42caff677678d92290cdc7017f040e2293d747940f112` |
| 内容 | 与 8/26 预置版相同，另在 `/data/wifi.cfg` 写入 SSID/PASSWORD，`study-terminal.sh` 开头启动 `wifi_manager &`（小智同款自连）。8/26 预置版不含这些，不会自动连 WiFi，已被取代。 |
| 烧录状态 | **未烧录。** 烧录后应开机自动连 iQOO。 |

### TLS SPKI 诊断版：构建/打包/备份完成，等待烧录取证

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_tls-spki-diag_20260815_256Mnand.img` |
| 镜像大小 | 38,977,536 bytes |
| 镜像 SHA-256 | `df8a9055233e5bbc726d91ec7888a8e800336f2a2e0b1b949a5264e40697395e` |
| 匹配 ELF | `archive/nuttx-20260815-tls-spki-diag.elf` |
| ELF 大小 | 68,525,304 bytes |
| ELF SHA-256 | `56155a4c212e7d3854e4c3a9b4e510c996250dc111b632c3aab6135090434537` |
| 构建检查 | 无 `error:`、无 undefined reference；Dragon 输出 `SUCCESS`。 |

### 城市/时区/音量修订版：历史配对备份

| 项目 | 值 |
|---|---|
| 镜像 | `firmware/rtos_nuttx_r528s3-dshanpi_citypicker-tz-volfix_20260814_256Mnand.img` |
| 镜像 SHA-256 | `5b10562bdd70515069fa3975738bfd78ee7775ba7ba4ef2fecd699bb4af448f5` |
| 匹配 ELF | `archive/nuttx-20260814-citypicker-tz-volfix.elf` |
| ELF SHA-256 | `8cdffbab9185e14ac8bd63c5573068c61bd477f1bd097beb7b117d2fd58e9a3f` |

## 建议的板端验收顺序

本轮优先验收**不依赖云端**的部分，联网功能留到凭据更新之后。

1. 核对本页最新候选的镜像与 ELF 校验值后，再按用户授权安排烧录。
   MiMo Key 已报告过期，先在本地更新有效凭据，勿将密钥粘贴到聊天或日志。
2. 冷启动验证开机听感、串口和基本触摸。
3. **离线部分（本轮重点）**：顶栏铃铛进“学习工具”，先确认工具页与主页是同一套视觉
   （深色顶栏 + 深色 tabbar + 白色半透明卡片，不再是浅色整屏）。设 30 秒休息提醒 →
   返回主页看倒计时 → 到期弹窗 → 点“5 分钟后再提醒”验证原弹窗保留到保存成功；
   再设一条一分钟提醒验证逐条取消；攒到几条后点“清除全部”，确认状态行如实报出
   条数且列表清空。列表加到 16 条，确认滚动条不压取消按钮、“清除全部”不挤压下拉框。
4. **专注记录与学习报告**：设 25 分钟专注，确认计时按实际经过时间走；完成后进
   “专注记录”页核对今日进度、七日柱状图和连续天数；再切到“学习报告”页，
   核对今日专注 / 完成轮次 / 连续天数 / 近 7 天累计与柱状图**完全一致**。
   断电重启后确认已保存数据仍在。
5. 验证快速松手、识别时返回、失败后重试。天气和时钟另行验收，不用某一项成功
   代替整体通过。
6. 云端部分（ASR / 聊天 / TTS）在有效凭据写入后再验；连续三轮对话、唤醒自动开页
   和播报仍待真机确认。

### ELF 配对的一处修正

`nuttx.bin`（进镜像的那份，SHA `fd417f2f...`）的真实产出 ELF 是 VM 上的 `nuttx/nuttx`，SHA `fa0593cd...`，已用 `cmp -n 11762444` 把 ELF 的 `.text` 与 `nuttx.bin` 前 11,762,444 字节逐字节比对通过。

而此前归档的 `archive/nuttx-20260828-...-popenstack.elf`（SHA `44bdeb56...`，68,516,936 bytes）与它文件级不同，**没有做过这个逐字节比对**。两者可能只差调试段，也可能是不同次链接；在没比对之前，解崩溃地址请用 `archive/nuttx-20260904-bootvoice2-verified-pair.elf`。

顺带修正：旧文档把 popenstack ELF 的大小写成 68,525,304 bytes，那是 `nuttx-20260815-tls-spki-diag.elf` 的大小，抄错行了。实际是 68,516,936 bytes。


## 不应再做的事

- 不继续物理音量键实验；该路线已被放弃。
- 不把 WAPI AP/captive portal 写成已实现。
- 不用静态 CJK 子集替代 FreeType 动态回退；AI 回复需要任意中文字符。
- 不把假时间、占位 IP、单次局部日志当作整条链路通过。
- 不删除或只备份镜像而不备份对应 ELF。
- **不用“字符串在镜像里”当作功能验证。** 8/28 的 bootvoice 版就是这么“验证”通过的，结果功能为零还搞坏了串口。命令行工具在打包前必须先读它的 `main()`，确认它真的读 argv。
- 不给 `nxplayer` 或 `mediatool` 传路径当参数；两个都是 stdin REPL，参数会被丢弃。要播放就重定向 stdin 喂命令，或用 `audio_playback.c` 的 `popen("nxplayer","w")` 写法。
- 不在后台 nxplayer 的命令序列里省掉 `!sleep` 或 `q`：省 `!sleep` 会被 `q` 掐断播放，省 `q` 会在 EOF 上无限打印 `nxplayer> `。
