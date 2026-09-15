# 真机复验清单 · 2026-09-15 唤醒门控与英文唤醒

> 前提：烧录 `firmware/rtos_nuttx_r528s3-dshanpi_wake-gate_20260915_256Mnand.img`
> 串口：`COM15` / **1500000** / 8N1（CH343）。`nsh>` 下先执行 `ai_agent` 进入 `vela>`。
>
> **红线**：不要输入、更不要录像 `config_show`（它会明文打印 API Key）。
> 录屏前先停止录制再改配置。

## 一、离线项（不用麦克风、不用网络、不用 Key）

这一组直接调 `voice_wake_matches()`，是**修复前 vs 修复后**对比最干净的证据。

| # | 命令 | 修复前实测 | 期望 |
|---|---|---|---|
| 1 | `voice_wake_test Hello，openvela` | yes | **yes** |
| 2 | `voice_wake_test Hello, openvela` | **no** | **yes** ← 本轮修复点 |
| 3 | `voice_wake_test hello openvela` | **no** | **yes** ← 本轮修复点 |
| 4 | `voice_wake_test Hello，OpenVela` | yes | **yes**（不回归） |
| 5 | `voice_wake_test 你好，openvela` | yes | **yes**（不回归） |
| 6 | `voice_wake_test 你好 openvela` | yes | **yes**（不回归） |
| 7 | `voice_wake_test 你好小米` | no | **no**（品牌词隔离） |
| 8 | `voice_wake_test 小米同学` | no | **no** |
| 9 | `voice_wake_test 你好` | no | **no** |

**#2 / #3 从 no 变 yes，是本轮改动在真机上生效的硬证据。**

## 二、门控项（关键：验证空转上传被消掉）

1. `voice_wake_start`
2. **保持安静 60 秒**，不要说话
3. 观察串口

| 观察点 | 修复前 | 期望（修复后） |
|---|---|---|
| `batch ASR (with_text): 80000 bytes` | 60 秒内约 14 次 | **0 次** |
| `vela_tls Handshake OK` | 约 14 次 | **0 次** |
| `batch ASR result:` | 一串幻觉文本 | **无** |

然后：

4. 对着麦克风清晰说 `Hello，openvela`（或 `你好，openvela`）
5. 期望串口出现一行：`endpoint gate: speech NNNms, uploading`，随后 `wake phrase matched`

**若第 2–3 步仍有周期性上传 → 门控没生效（阈值太松）。**
**若第 4–5 步说了话却不上传 → 阈值太严。** 两种情况都要回报，我来调
`track_utterance()` 的判据（当前：均值 ≥ 250 且峰值 ≥ 1200，持续 ≥ 200 ms）。

## 三、英文语音识别项（验证 `language=auto`）

`voice_wake_test` 只能证明**匹配**，证明不了**识别**。要验识别，走这条链：

```
voice_capture_test 4          ← 提示 "Recording 4 seconds" 后，清晰说 "Hello，openvela"
voice_test_asr /data/ai_agent/mic-capture.pcm
```

串口会打印 `ASR result: <识别文本>`。**请把这一行原样回报。**

| 现象 | 含义 |
|---|---|
| `ASR result:` 里出现 `Hello` / `hello` 之类拉丁字母 | ✅ 修复生效 |
| `ASR result:` 是中文谐音（如 `哈喽 openvela`） | ❌ 仍是中文解码，需再查 |

> 这是本轮最关键的一条：它是「英文唤醒词检测不到」的直接判据。
> `voice_test_asr` 与唤醒监听走**同一条 ASR 代码路径**，所以它能代表唤醒时的识别行为。

## 四、播报项（验证"能不能说话"）

| # | 命令 | 期望 |
|---|---|---|
| 1 | `voice_test_speak 语音播报测试` | 能出声，串口 `MiMo TTS playback OK.` |
| 2 | `voice_wake_start` 后再执行 #1 | 仍能出声（两项互不阻塞） |

串口正常长这样（`media_player_open failed` 是**预期告警**，不是故障）：

```
[audio_pb] media_player_open failed
[audio_pb] media unavailable; spooling PCM for nxplayer (24000Hz 1ch 16bit)
[voice] TTS network done: 3318ms
[audio_pb] nxplayer fallback playback: 1760ms
nxplayer> playraw /data/ai_agent/tts_stream.pcm 1 16 24000 0
[audio_pb] nxplayer fallback finished: status=0 bytes=84480
MiMo TTS playback OK.
```

## 四之二、按键对话（PTT）项

本轮还修掉一个**会真的出问题**的漏改：`app/hello_app/voice_ui_bridge.c` 里
`voice_channel_stop_with_text()` 的声明与调用**只传了两个参数**
（该文件不包含 ai_agent 的头文件，自己写了一份原型，加第三个参数时被漏改）。
它**是参与构建的**，所以第三个参数会取寄存器里的残留值 ——
门控在 PTT 路径上会**随机开或关**。

| # | 操作 | 期望 |
|---|---|---|
| 1 | 按住 PTT 键，清晰说一句话，松开 | 正常识别并回复（PTT 路径**不门控**，与改动前一致） |
| 2 | 按住 PTT 键，**不说话**，约 1 秒后松开 | 会照常发起一次识别 —— **这是预期行为**，不是缺陷 |
| 3 | 观察串口 | 不应出现 `endpoint gate: silence, ASR upload skipped`（该行只属于唤醒路径） |

> 第 2 条是**设计选择**：PTT 是用户主动按键，故不做门控。若希望 PTT 也跳过静音，
> 把 `false` 改成 `true` 即可（一行）。

## 五、回报格式（照抄即可）

```
【一 离线项】1=yes 2=__ 3=__ 4=__ 5=__ 6=__ 7=__ 8=__ 9=__
【二 门控】静置60秒上传次数=__ ；说话后是否出现 endpoint gate: speech=是/否
【三 英文识别】ASR result: ____________________
【四 播报】能出声=是/否 ；唤醒开启时能出声=是/否
【四之二 PTT】按说能识别=是/否 ；静音按下会否上传=是/否（预期"是"）
【其它】异常日志原样贴回
```

## 六、已知且不必报告的现象

| 日志 | 说明 |
|---|---|
| `[audio_pb] media_player_open failed` | 预期。本板媒体服务路径不可用，播放走 nxplayer 回退，能出声。 |
| `[voice_asr] Active ASR backend has no streaming support` | 预期。MiMo ASR 无流式接口，代码自动回退批量模式。 |
| `[voice] pre-open ASR failed, will retry in thread` | 同上，流式预连接失败后的正常回退。 |
| `[vela_tls] System clock not set (UNIX=1822)` | **不是故障**。`authmode=VERIFY_OPTIONAL`，证书有效期不参与握手（`vela_tls.c:313-331`）。 |
| `[ws] bind() failed on port 28789, errno=98` | 端口被占用，WS 通道未起。与本轮改动无关，不影响语音与对话。 |
