# 烧录清单：门控修复（2026-09-16）

> 本轮只改了一件事：**让"静音窗口"真的不上传**。
> 串口：`COM15` / **1500000** / 8N1（CH343）。`nsh>` 下先敲 `ai_agent` 进 `vela>`。
>
> **红线**：不要输入、更不要录像 `config_show`（它明文打印 API Key）。

## 0. 先核对烧的是不是这一版

```text
镜像：firmware/rtos_nuttx_r528s3-dshanpi_gate-absolute_20260916_256Mnand.img
大小：40845312 bytes
SHA-256：8296a1284161445936d0777f0fcd7e1e95866f316a987028340d5733a34fec93
```

配套 ELF（仅备查，不用烧）：
`firmware/nuttx-gate-absolute_20260916.elf`，68,686,580 bytes，
`10ea9b662de37eddac5d391531e7b0df6d95341d4c9d191cec74d85264f1c277`。

烧录前先在电脑上核对 SHA-256，再用 **PhoenixSuit**
（`D:/down/AllwinnertechPhoeniSuit/AllwinnertechPhoeniSuitRelease20201225/PhoenixSuit.exe`）。
板子断电 → USB 接烧录口 → 按住 FEL 键上电 → 选固件 → 一键刷机。

> ⚠️ 镜像含明文预置的 Key 与 WiFi 密码，只在本机烧，**不要截图外发、不要上传**。

## 1. 为什么还要再烧一次

上一轮（09-15）修的四条已经生效——本轮真机复验，9 条离线唤醒用例全过，
`Hello, openvela` 和 `hello openvela` 都从 `no` 翻成 `yes`。

**但门控没生效**：`voice_wake_start` 后静置 65 秒，实测 **16 次 ASR 上传 + 16 次 TLS 握手**
（每 4.3 秒一轮），而 `endpoint gate: silence` 一次都没打。串口里那行

```
[voice] endpoint gate: speech 0ms, uploading
```

是铁证：门控读到"累计有声 0 毫秒"却放行了。原因是判据用的是 `speech_seen`，
而它在整个窗口里是**黏的**（一有 200 ms 噪声就置位，之后不安静也不清零），
每轮开头那段采集瞬态（峰值 11778–32768）刚好够把它拉起来。

**这就是你说的「开启唤醒词他就一直识别语音转文字」的确切原因。**
本轮把它换成"窗口内累计有声时长"这一个绝对判据（默认 500 ms），
并把阈值做成运行时可调。

## 2. 复验一：门控（本轮重点）

```
voice_wake_start
```

然后**保持安静 60 秒**，一个字都不说。看串口：

| 观察点 | 上一轮实测 | 本轮期望 |
|---|---|---|
| `batch ASR (with_text)` | 16 次 / 69 s | **0 次** |
| `vela_tls Handshake OK` | 16 次 | **0 次** |
| `endpoint gate: silence voiced=…` | 0 次 | **1 行**（状态跳变才打） |
| `batch ASR result:`（幻觉文本） | 16 条 | **无** |

期望看到的那一行长这样：

```
[voice] endpoint gate: silence voiced=200ms of 2500ms, ASR upload skipped
```

**把这一行原样回报** —— 里面 `voiced=…ms of …ms` 就是调阈值需要的数字。

### 2.1 如果仍然在空转：不用重烧，直接调

阈值运行时可改，**改完立刻生效**：

```
voice_gate                       # 看当前值
Endpoint gate: min_voiced_ms=500
voice_gate 800                   # 把门槛提到 800ms
voice_wake_start                 # 重新听 60 秒
```

规则：
- 静音还在上传 → 把 `min_voiced_ms` 调到那行报的 `voiced` **之上**；
- 说了话反而不上传（见第 3 节）→ 把 `min_voiced_ms` 调**低**。

**调好的值请告诉我，我写进代码当默认值。**

### 2.2 说话必须仍然能过

```
voice_wake_start
# 清晰说：「你好，openvela」
```

期望：
```
[voice] endpoint gate: speech voiced=1200ms of 2500ms, uploading
[voice_wake] wake phrase matched
```

| 现象 | 含义 |
|---|---|
| 出现 `speech …` + `wake phrase matched` | ✅ 门控正常 |
| 说了话但 `endpoint gate: silence` | ❌ 阈值太严，按 2.1 调低 |

## 3. 复验二：离线唤醒词（应当与上一轮一致，用于确认没回归）

```
voice_wake_test Hello，openvela
voice_wake_test Hello, openvela
voice_wake_test hello openvela
voice_wake_test Hello，OpenVela
voice_wake_test 你好，openvela
voice_wake_test 你好 openvela
voice_wake_test 你好小米
voice_wake_test 小米同学
voice_wake_test 你好
```

期望：前 6 条 `yes`，后 3 条 `no`（**2026-09-16 已实测全过，这里只确认没回归**）。

## 4. 关于唤醒词：中文已经是官方形式，不用改

你说「唤醒词换成中文的吧」——查了仓库记录的官方规则原文：

> 唤醒词：**「你好，openvela」/「Hello，openvela」**

`voice_wake.c` 的匹配表上方也写着同一条规则。也就是说：

- **中文「你好，openvela」本来就是官方主形式**，而且已经真机通过；
- 英文「Hello，openvela」是规则里**并列的第二种形式**，删掉会掉合规，所以保留。

你要的效果（说中文就能唤醒）**现在就已经满足**，不需要改代码。

## 5. 复验三：TTS（本轮已实测 8/8 通过，可选复测）

```
voice_test_speak 语音播报测试
```

期望能出声，串口 `MiMo TTS playback OK.`。

> 本轮做了 4 分钟压测：64 次 ASR 上传之后 TTS **仍然成功**；
> 单独重复播报 **8/8 成功**（3971–5615 ms）。
> 所以「不能说话」不是 TTS 坏了，而是设备一直卡在空转、走不到说话那一步。

## 6. 回报格式（照抄即可）

```
【0 镜像】SHA-256 核对=一致/不一致
【2 门控】静置60秒上传次数=__ ；endpoint gate: silence 那行=____________
【2.1 调参】是否调过=__ ；调成=__
【2.2 说话】出现 speech + wake phrase matched=是/否
【3 离线】1=__ 2=__ 3=__ 4=__ 5=__ 6=__ 7=__ 8=__ 9=__
【5 TTS】能出声=是/否
【其它】异常日志原样贴回
```

## 7. 已知且不必报告的现象

| 日志 | 说明 |
|---|---|
| `[audio_pb] media_player_open failed` | 预期。播放走 nxplayer 回退，能出声。 |
| `[voice_asr] Active ASR backend has no streaming support` | 预期。MiMo ASR 无流式接口，自动回退批量模式。 |
| `[voice] pre-open ASR failed, will retry in thread` | 同上，正常回退。 |
| `[vela_tls] System clock not set (UNIX=1822)` | **不是故障**。`authmode=VERIFY_OPTIONAL`，证书有效期不参与握手。 |
| `[voice] chunk#1: peak=11778` 之类的瞬态 | 采集通路起始瞬态，**门控现在会正确忽略它**。 |
