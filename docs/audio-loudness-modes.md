# 音频「高动态 / 标准模式」（音量均衡）C++ 实现可行性说明

> 问题来源：Web 播放器（hili-player）有音频插件，提供「音量均衡」三档（关闭 / 标准 / 高动态）。
> 本文档回答：C++（CicadaPlayerNext）能否实现同样的能力？是否需要第三方库？

---

## 1. 结论（TL;DR）

**能实现，且不需要任何第三方库。**

本工程自带的 FFmpeg 9.0（`external/external/ffmpeg`，`RELEASE` = 9.0 "Lei"）的 libavfilter
里已经有 `acompressor`（软拐点向下压缩器），它：

1. **参数与 Web 端 `DynamicsCompressorNode` 一一对应**
   （threshold / knee / ratio / attack / release，外加 makeup 补偿增益）；
2. **全部参数支持运行时热更新**——本工程 FFmpeg 9.0 源码里该滤镜的每个选项都带
   `AV_OPT_FLAG_RUNTIME_PARAM` 标志，且注册了 `process_command`
   （`external/external/ffmpeg/libavfilter/af_sidechaincompress.c:75-95` 选项表、
   `:219` process_command、`:427-436` 滤镜注册），
   即切换模式**不需要重建滤镜图**，走 `avfilter_graph_send_command` 即可——
   与现有 volume / atempo 的热更新是**同一套机制、同一个先例**
   （`framework/filter/ffmpegAudioFilter.cpp:72` tempo 命令、`:83` volume 命令）；
3. **挂接点现成**：所有平台音频渲染器共用基类 `filterAudioRender`
   （`framework/render/audio/filterAudioRender.h:16`）内部已经持有
   `ffmpegAudioFilter`（`filterFactory::createAudioFilter` 创建），加一个
   loudness 通道即可，桌面（QtPlayer 走 `SdlAFAudioRender2 : filterAudioRender`）、
   macOS/iOS（AudioQueue / AudioUnit）、Android（AudioTrack / AAudio）**一处实现、全平台生效**。

一句话：**自研 DSP 或引第三方库都没有必要**；工作量集中在「参数换算 + 一条 setOption 通道 +
上层 API 接线」，预估净改动 5 个文件（见 §6）。

---

## 2. Web 端参考实现（hili-player）

### 2.1 调用链

```
设置面板「音量均衡」
  → VideoPlayer.setLoudness 广播 AUDIO_EFFECT_CHANGE({effect:'loudness', mode})
  → AudioEffectPlugin.setLoudness(mode)               （packages/plugins/src/audioeffect/AudioEffectPlugin.ts:245）
  → EffectChain.setLoudness(mode)                     （packages/plugins/src/audioeffect/effects/EffectChain.ts:196）
  → CompressorEffect（DynamicsCompressorNode 五参数） （packages/plugins/src/audioeffect/effects/CompressorEffect.ts）
```

- mode 语义：**0 关闭 / 1 标准 / 2 高动态**。
- EffectChain 的处理顺序（`EffectChain.ts:196-211`）：
  - `mode === 0` → 停用压缩器（并联结构里断开该支路，恢复干声直连 `source → gain`）；
  - `mode === 1 / 2` → **先 `setData` 应用对应模式参数、再 `setEffectActive` 激活**——
    先参数后激活，避免激活瞬间以 default 参数短暂生效造成听感跳变；
- 参数应用方式（`CompressorEffect.ts:40-48`）：五个 AudioParam 全部
  `setValueAtTime(value, now + 0.1)` 短延迟过渡，避免参数突变引发爆音
  （C++ 侧对应的注意点见 §6 注意点 ③）。

### 2.2 预设参数（`packages/plugins/src/audioeffect/presets.ts:123-145`）

| 预设 | threshold | knee | ratio | attack | release |
|---|---|---|---|---|---|
| default（= Web Audio 节点默认值） | -24 dB | 30 dB | 12 | 0.003 s | 0.25 s |
| **loudnessStandard（标准）** | **-18 dB** | **24 dB** | **12** | **0.003 s** | **0.25 s** |
| **loudnessDynamic（高动态）** | **-32 dB** | **12 dB** | **4** | **0.01 s** | **0.4 s** |

设计意图（presets.ts:115-122 注释原文）：
- **标准**：较高阈值提前触发 + 高比率强压缩，把忽大忽小的节目响度拉平（对白 / 夜间观看友好）；
- **高动态**：深阈值只压峰值 + 低比率温和压缩，保留节目的动态起伏（音乐 / 电影场景）。

### 2.3 Web 端节点图（供对照）

```
mediaSource ──► (active 效果支路) ──► gain ──► destination
            └─► 全部 inactive 时干声直连 gain ──► destination
```
效果链为**并联**结构（`EffectChain.ts:1-18` 头注释）：EQ / Reverb / A3D / Phone / Compressor
各自独立从 mediaSource 接入；压缩器关闭 = 物理断开该支路。
C++ 侧没有"物理断开"的必要——`acompressor` 在 `ratio=1` 时数学上恒等直通（见 §4.3）。

---

## 3. C++ 侧现状（本工程音频管线）

```
QtPlayer CicadaPlayerItem::setVolume / setSpeed          （platform/QtPlayer/src/CicadaPlayerItem.cpp:1339/1367）
  → ICicadaPlayer::SetVolume / SetSpeed
  → SuperMediaPlayer
  → SMPAVDeviceManager::setVolume                         （mediaPlayer/SMPAVDeviceManager.cpp:222-225；
                                                            渲染器在 :303 经 AudioRenderFactory::create() 创建）
  → filterAudioRender::setVolume / applyVolume            （framework/render/audio/filterAudioRender.cpp:191/370）
  → ffmpegAudioFilter::setOption("volume", …, "volume")   （framework/filter/ffmpegAudioFilter.cpp:78-87）
  → avfilter_graph_send_command(graph, "volume", "volume", …)   （:83 —— 热更新先例）
```

`ffmpegAudioFilter::init()` 构建的滤镜图（`ffmpegAudioFilter.cpp:175-256`）：

```
abuffer ──► [volume（仅 A_FILTER_FLAG_VOLUME）] ──► [atempo（仅 A_FILTER_FLAG_TEMPO）] ──► [aformat（按需）] ──► abuffersink
```

- 现有 flag 位：`A_FILTER_FLAG_TEMPO = (1 << 1)`、`A_FILTER_FLAG_VOLUME = (1 << 2)`
  （`framework/filter/IAudioFilter.h:15-16`）——**下一个空闲位是 `1 << 3`**；
- `filterAudioRender::init` 目前恒置 `TEMPO | VOLUME`（`filterAudioRender.cpp:57-64`）；
- 滤镜实例是**懒创建**的：仅在格式转换（`needFilter`）、`applySpeed`、或音量 > 1 需要
  软件放大时才建图（`filterAudioRender.cpp:78-85 / 353-368 / 370-402`）；
- 所有平台渲染器都继承 `filterAudioRender`：SdlAFAudioRender2（桌面/QtPlayer）、
  AFAudioQueueRender / AFAudioUnitRender（Apple）、AudioTrackRender / AaudioRender（Android）、
  CheaterAudioRender；只有 OHOS 的 `OhosAudioRender` 直接实现 `IAudioRender`（鸿蒙侧
  需单独接线，不影响结论）。

---

## 4. FFmpeg 9.0 `acompressor` 关键事实（源码实证）

以下全部直接读自本工程 FFmpeg 源码
`external/external/ffmpeg/libavfilter/af_sidechaincompress.c`（acompressor 与
sidechaincompress 共用实现）。

### 4.1 参数表（`:75-95`，全部带 `A|F|R` —— R 即运行时可改）

| 参数 | 类型/单位 | 范围 | 默认 | 说明 |
|---|---|---|---|---|
| `level_in` | 线性增益 | 0.015625–64 | 1 | 输入增益，保持 1 |
| `mode` | 枚举 | downward / upward | downward | 向下压缩（= Web 行为），保持默认 |
| `threshold` | **线性幅度** | 0.000976563–1 | 0.125 | 对应 **-60 dB–0 dB**；内部取对数 `s->thres = log(threshold)`（`:148`） |
| `ratio` | 倍率 | 1–20 | 2 | 与 Web ratio 同义同范围 |
| `attack` | **毫秒** | 0.01–2000 | 20 | 起Attack 时间 |
| `release` | **毫秒** | 0.01–9000 | 250 | 释放时间 |
| `makeup` | **线性增益** | 1–64 | 1 | 输出补偿增益（Web 节点无此显式参数） |
| `knee` | **几何比** | 1–8 | 2.82843 | 见 §4.2，与 Web 的 dB 语义不同，需换算 |
| `link` | 枚举 | average / maximum | average | 多声道联动方式 |
| `detection` | 枚举 | peak / rms | **rms** | 检测器，默认 rms 与 Web 实现取向一致 |
| `mix` | 0–1 | — | 1 | 干湿比，保持 1（全压缩信号） |

### 4.2 knee 的语义差异（唯一需要"换算而非照抄"的参数）

FFmpeg 的 knee 不是 dB 宽度，而是**围绕阈值的几何比**（`compressor_config_output`，`:149-154`）：

```
knee 区间（线性幅度） = [ threshold / √knee , threshold × √knee ]
换算成 dB 总宽度     = 20·log10(knee)
```

- 默认 2.82843 → 宽度 20·log10(2.82843) ≈ 9.03 dB；
- 上限 8 → 最宽 ≈ 18.06 dB；
- 且拐点区**对称跨在阈值两侧**；Web Audio 规范的 knee 区间在**阈值上方**。
  → 形状近似、**非逐位一致**（对听感影响很小，见 §7）。

**换算公式：`knee_ffmpeg = 10^(knee_web_dB / 20)`，再夹取到 [1, 8]。**

### 4.3 `ratio=1` 即数学直通（"关闭"不需要拆图）

`output_gain()`（`:107-141`）：`gain = (slope - thres) / ratio + thres`。
当 `ratio = 1` 时 `gain = slope`，返回 `exp(gain - slope) = 1`——
包括软拐点区间在内（ratio=1 时 hermite 插值两端点与斜率全部退化为恒等映射）。
即：**mode 0（关闭）= 把 acompressor 置为 ratio=1（可再加 knee=1 双保险）**，
与 Web 端"干声直连"听感等价，且免去图重建（拆图会丢滤镜内部缓冲的几十毫秒音频）。

### 4.4 运行时热更新通道

滤镜注册带 `.process_command = process_command`（`:435`），配合全参数 R 标志，
`avfilter_graph_send_command(graph, "acompressor", "threshold", "0.125893", …)`
即可逐参数热切换——和现有 volume（`ffmpegAudioFilter.cpp:83`）、
atempo（`:72`）完全同一机制。

---

## 5. 参数映射表（核心交付物）

Web `DynamicsCompressorNode` → FFmpeg `acompressor` 换算规则：

| Web 参数 | 单位 | FFmpeg 参数 | 换算 |
|---|---|---|---|
| threshold | dB | threshold | 线性 = `10^(dB/20)` |
| ratio | 倍率 | ratio | 原样（范围一致 1–20） |
| attack | 秒 | attack | × 1000（秒→毫秒） |
| release | 秒 | release | × 1000（秒→毫秒） |
| knee | dB | knee | `10^(dB/20)` 夹取 [1,8] |
| —（无） | | makeup | 建议从 1 起步（见注②） |
| —（无） | | detection / link / mix / mode / level_in | 保持默认 rms / average / 1 / downward / 1 |

三档模式的**换算结果（可直接抄进代码）**：

| 档位 | threshold | knee | ratio | attack | release | makeup |
|---|---|---|---|---|---|---|
| **0 关闭** | （保持当前值） | 1 | **1** | （保持） | （保持） | 1 |
| **1 标准** | `10^(-18/20)` = **0.125893** | `10^(24/20)`=15.85 → **8**（夹上限） | **12** | **3** ms | **250** ms | 1（注②） |
| **2 高动态** | `10^(-32/20)` = **0.025119** | `10^(12/20)` = **3.981** | **4** | **10** ms | **400** ms | 1 |

即滤镜串（init 时构建、关闭态）：

```
acompressor=threshold=0.125893:knee=1:ratio=1:attack=3:release=250:makeup=1:detection=rms:link=average:mix=1
```

> 注①：标准档 threshold 换算值 0.125893 与 FFmpeg 默认值 0.125（≈-18.06 dB）几乎重合，
> 属巧合但说明该档参数在 FFmpeg 参数空间的"常规区"。
>
> 注②：Web 的 `DynamicsCompressorNode` 内部行为（含补增益）由 W3C 规范固定，不暴露
> makeup 参数；FFmpeg 侧强压缩（ratio=12）会明显拉低输出响度，若 A/B 对比发现标准档
> 偏轻，可给标准档试 `makeup=1.4`（≈ +3 dB）到 `2`（+6 dB）。**先 1 后调**，不要凭空补偿。

---

## 6. 建议的集成改动（按依赖顺序 5 步）

### ① `framework/filter/IAudioFilter.h`
新增 flag 位：

```cpp
#define A_FILTER_FLAG_LOUDNESS (1 << 3)   // 现有 TEMPO=(1<<1)、VOLUME=(1<<2)
```

### ② `framework/filter/ffmpegAudioFilter.{h,cpp}`

- 头文件加成员：`int mLoudnessMode = 0;`（记当前档位，init 重建图时恢复）；
- `init()`：`flags & A_FILTER_FLAG_LOUDNESS` 时，在 volume 段之后、atempo 段之前
  `addFilter(&current, "acompressor", "threshold=0.125893:knee=1:ratio=1:attack=3:release=250:makeup=1")`
  （**关闭态常驻图**：ratio=1 直通，切换永远零重建）；
- `setOption()` 加分支：

```cpp
} else if (capacity == "loudness") {
    if (key == "mode") {
        int mode = atoi(value.c_str());
        if (mode != mLoudnessMode) {
            std::lock_guard<std::mutex> uMutex(mMutexRate);   // 与 atempo 同一把锁（:68 先例）
            mLoudnessMode = mode;
            if (m_pFilterGraph) {
                applyLoudnessToGraph();   // 按 §5 表逐参数 send_command
            }
        }
        return true;
    }
}
```

  `applyLoudnessToGraph()` 内部对五个参数各发一条
  `avfilter_graph_send_command(m_pFilterGraph, "acompressor", "threshold", "0.125893", nullptr, 0, 0)`
  （参数名与值即 §5 表）。

### ③ `framework/render/audio/{IAudioRender.h, filterAudioRender.h/.cpp}`

- `IAudioRender` 加虚方法（默认 `-ENOTSUP`，非 filterAudioRender 渲染器不受影响）：

```cpp
virtual int setLoudness(int mode) { return -ENOTSUP; }
```

- `filterAudioRender::setLoudness(int mode)`：仿 `applySpeed`（`filterAudioRender.cpp:353-368`）——
  图未建时先存值并建图（`createAudioFilter` + `init`，**注意 init 前先
  `mFilter->setOption("mode", …, "loudness")` 不行——setOption 只在图存在时发命令，
  未建图时值记在 mLoudnessMode，init 里按 §5 表直接构建对应档参数**），
  图已存在时直接 `mFilter->setOption("mode", …, "loudness")`；
- `init()` 的 `mFilterFlags` 追加 `A_FILTER_FLAG_LOUDNESS`（滤镜常驻、ratio=1 直通，
  单核开销 <0.1%，见 §7 性能）。

### ④ `mediaPlayer/`（SuperMediaPlayer / SMPAVDeviceManager）

- `SMPAVDeviceManager::setLoudness(int mode)` 转发 `mAudioRender->setLoudness(mode)`
  （与 `setVolume` 同构，`SMPAVDeviceManager.cpp:222-225`）；
- `SuperMediaPlayer` / `ICicadaPlayer` 暴露：
  或走既有选项通道 `SetOption("audio-loudness", "0/1/2")`，或加专用
  `SetLoudness(int)`（与 SetVolume/SetSpeed 并列，QtPlayer 已有同构调用先例
  `CicadaPlayerItem.cpp:1339/1367`）。

### ⑤ `platform/QtPlayer/src/CicadaPlayerItem.{h,cpp}`

- QML 属性 `loudnessMode: int` + `setLoudness(qreal)` 槽，写透到 `m_player`；
  设置面板加三档开关（与 Web 设置面板「音量均衡」对齐）。

### 注意点

1. **线程安全**：`avfilter_graph_send_command` 与 FilterLoop 线程并发访问 graph
   不是线程安全的——atempo 分支已用 `mMutexRate` 保护（`ffmpegAudioFilter.cpp:68-73`），
   loudness 必须走同一把锁（volume 分支没加锁属历史遗留，勿仿照）。
2. **懒建图时机**：滤镜只在 needFilter / applySpeed / applyVolume(>1) 时才存在
   （§3）。用户直接开 loudness 而图未建时，需仿 `applySpeed` 主动建图
   （`filterAudioRender.cpp:355-358`）。
3. **防爆音**：Web 端用 `setValueAtTime(now+0.1)` 过渡（§2.1）。acompressor 的
   attack/release 包络本身有平滑（`:158-159` 系数），参数阶跃通常无爆音；若实测
   切换有咔哒声，可在两档参数间做 50–100 ms 的线性插值（Timer 分几步发命令）。
4. **flush/seek**：图 flush → 重建后 acompressor 包络状态自动复位，无需额外处理；
   `mLoudnessMode` 记忆在滤镜对象里，重建后 init 按当前档构建，行为自洽。
5. **OHOS**：`OhosAudioRender` 不继承 filterAudioRender（§3），鸿蒙要单独接
   （它若支持滤镜需要自行挂 ffmpegAudioFilter）；桌面/Apple/Android 不受影响。

---

## 7. 与 Web 端的差异 & 验收建议

| 差异点 | 影响 | 建议 |
|---|---|---|
| knee 形状（对称跨阈值 vs 阈值上方） | 拐点区位置不同，过渡曲线近似非逐位一致 | 听感无感知差异；标准档 knee 已夹到上限 8 |
| 检测器实现 | acompressor rms 检测 + hermite 拐点插值 vs WebAudio 规范曲线 | detection 默认 rms 与 Web 取向一致，保持默认 |
| makeup | Web 规范内部固定，FFmpeg 显式参数 | 先 makeup=1，A/B 后按 §5 注②微调 |
| 参数过渡 | Web setValueAtTime 延迟 0.1s vs send_command 阶跃 | attack/release 自带平滑；必要时按 §6 注意点 ③ 插值 |

**验收方法**：同一段动态范围大的素材（如安静对白 + 爆炸场景的电影片段），
Web demo 与 QtPlayer 各录一遍输出（QML 侧可用系统回环录音），三档各对比一次：
关闭档应与原始输出波形一致；标准档对白响度抬升、爆炸被压；高动态档对白轻微抬升、
爆炸基本保留。参数表是"忠实映射"而非"逐位等价"，最终以 A/B 听感验收为准。

**性能**：acompressor 为逐样本 double 运算，48 kHz 立体声下远小于单核 0.1%；
且滤镜常驻（关闭态 ratio=1 直通）避免了档位切换时的建图/拆图开销与音频间隙。

---

## 8. 备选方案对比（为什么都不选）

| 方案 | 结论 | 理由 |
|---|---|---|
| **acompressor（本文方案）** | **采用** | 参数一一对应、全参数运行时可热改、零新增依赖、现有管线直接挂接 |
| 自写 DSP 压缩器（~百行包络 + 增益计算） | 不必要 | 唯一收益是可逐位复刻 WebAudio 规范曲线；维护成本 > 收益 |
| FFmpeg `loudnorm` | 不适用 | 是 EBU R128 **响度归一化**（整段拉到目标 LUFS），非动态压缩；单遍模式有数秒启动延迟，不适合实时三档切换 |
| FFmpeg `dynaudnorm` | 不适用 | 自适应增益归一化，行为模式与压缩器不同 |
| FFmpeg `alimiter` | 不适用 | 砖墙限幅器（ratio=∞ + 硬拐点），是压缩的极端形态；可作日后"防削波"补充，不是本需求 |
| 第三方 DSP 库（KFR、maximilian 等） | 不采用 | 引依赖、增加构建/授权/维护负担，而需求用现有 FFmpeg 已全覆盖 |

---

## 9. 附录：证据索引

| 事实 | 位置 |
|---|---|
| FFmpeg 版本 9.0 | `external/external/ffmpeg/RELEASE` |
| acompressor 实现/注册 | `external/external/ffmpeg/libavfilter/af_sidechaincompress.c:427-436` |
| 全参数 R（运行时）标志 + 范围 | 同上 `:75-95` |
| process_command 支持 | 同上 `:219`（实现）、`:435`（注册） |
| knee 几何比语义 | 同上 `:149-154`（`compressor_config_output`） |
| ratio=1 恒等直通 | 同上 `:107-141`（`output_gain`） |
| 现有热更新先例（volume/atempo 命令） | `framework/filter/ffmpegAudioFilter.cpp:72,83` |
| 音频滤镜图构建 | `framework/filter/ffmpegAudioFilter.cpp:175-256` |
| flag 位定义 | `framework/filter/IAudioFilter.h:15-16` |
| 渲染器基类持滤镜 | `framework/render/audio/filterAudioRender.h:113`、`filterAudioRender.cpp:78-85` |
| 懒建图先例（applySpeed/applyVolume） | `framework/render/audio/filterAudioRender.cpp:353-402` |
| 播放管线转发（volume） | `mediaPlayer/SMPAVDeviceManager.cpp:222-225,303` |
| QtPlayer 调用先例 | `platform/QtPlayer/src/CicadaPlayerItem.cpp:1339,1367` |
| Web setLoudness 语义 | `front/hili-player/packages/plugins/src/audioeffect/AudioEffectPlugin.ts:245`、`effects/EffectChain.ts:196-211` |
| Web 预设参数 | 同目录 `presets.ts:123-145` |
| Web 五参数节点 | 同目录 `effects/CompressorEffect.ts:40-48` |
