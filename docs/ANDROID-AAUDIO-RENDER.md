# Android 音频输出：AAudio 渲染器（默认 AAudio，不支持则回退现有 AudioTrack）

> 状态：**已核实参考实现 + 方案已定，待施工**。
> 参考源码：`D:\hilihili\mpv`（GitCode 镜像 `https://gitcode.com/gh_mirrors/mp/mpv.git`，
> `--depth 1` + sparse-checkout `audio/out`），关键文件 `audio/out/ao_aaudio.c`（509 行）。

## 一、为什么选 mpv 的这条实现做参考

mpv 是"纯 native 播放器"派（音频路径里没有 JVM），它的 AAudio 输出经过长期真机检验；
更关键的是它**自带"不支持就回退"的机制**，与本工程"按平台能力路由"的做法完全同构：

| mpv 的做法（file:line） | 含义 |
|---|---|
| `load_lib_functions()`：`android_get_device_api_level()` + `dlopen("libaaudio.so", RTLD_NOW\|RTLD_GLOBAL)`，再按 API 级别表逐符号 `dlsym`（失败再 `dlsym(RTLD_DEFAULT)`）——`ao_aaudio.c:191-217`，符号表在 `aaudio_functions26/28/32.inc` | **API<26 或符号缺失 ⇒ 初始化失败 ⇒ 自动换下一个 AO**（即我们的"回退"） |
| 流配置：`setDeviceId` / `setDirection(OUTPUT)` / `setSharingMode` / `setFormat`(I16/I32/FLOAT/IEC61937) / `setSampleRate` / `setBufferCapacityInFrames` / `setPerformanceMode`(none/low-latency/power-saving) / `setDataCallback` / `setErrorCallback`，可选 `setChannelMask`(含 FCC 上限表 `aaudio_max_chnums`) / `setUsage(MEDIA)` / `setContentType` / `setSessionId`——`:289-355`、`:150-189` | 配置面清单，照抄语义即可（我们只需要 I16 + MEDIA + 无 deviceId 指定） |
| **数据回调**：`AAudioStream_getFramesWritten()` + **`AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &presented, &present_time)`**，取不到就沿用上次值（`:228-244`） | 这是"**消费端已播出的帧数**"，正好当我们的音频时钟读数（比 `AudioTrack.getPosition()` 更准，且是硬件时间戳） |
| 运行控制：`requestStart` / `requestPause` / `requestFlush`（`:402`、`:414`、`:449`、`:463`） | 与我们 `IAudioRender` 的 start/pause/flush 一一对应 |
| 错误回调：`error_callback` → `ao_request_reload(ao)`（`:219-226`） | 设备切换/致命错误 → **整体重载输出**（干净的自愈模型，无计时器） |

**许可证提醒（必须知道）**：mpv 主体是 **GPLv2+**。把 `ao_aaudio.c` **逐字 copy** 进本工程会让整个
项目受 GPL 约束。因此本方案**只参考其行为与 API 用法，代码由我们自己写**（这也才符合本工程的
接口/注释/工程硬约束）。若你明确接受 GPL，可另行讨论；默认按"自研实现"走。

## 二、映射到本工程的实现方案

新增 `framework/render/audio/Android/AaudioRender.{h,cpp}`，实现既有 `IAudioRender`
（与 `AudioTrackRender` 同一接口），并注册进 `audioRenderPrototype`（`audioRenderPrototype.cpp:15`
按注册顺序取第一个 `is_supported()` 为真的实现 ⇒ **先注册 AAudio，它自报不支持时自然落到 AudioTrack**，
这就是"默认 AAudio、不支持回退"，且不是配置开关）。

要点（逐条对应 mpv 的做法）：

1. **能力判定**（等价于 mpv 的 `load_lib_functions` 失败即退出）：`dlopen("libaaudio.so")` +
   `dlsym` 必需符号（`AAudio_*`/`AAudioStreamBuilder_*`/`AAudioStream_*`），任一失败 ⇒
   `is_supported()` 返回 false ⇒ AudioTrack 接管。**不需要** `.inc` 那种按 API 级别分段表：
   我们只用一个"必需符号集"，取不到就整体回退（比 mpv 更简单，因为不需要支持 API 26 以下的 AAudio）；
2. **流配置**：`setDirection(OUTPUT)`、`setFormat(PCM_I16)`（与现有渲染链输出一致：S16）、
   `setSampleRate(输出采样率)`、`setChannelMask(按声道数)`、`setUsage(MEDIA)`、
   `setPerformanceMode(LOW_LATENCY)`（拿不到低延迟就 `NONE`，由平台自己决定）、
   `setBufferCapacityInFrames(2 × 目标缓冲)`、`setDataCallback/setErrorCallback`；
3. **数据回调** = 我们的 `write_loop` 等价物：从 `mFrameQueue` 取 PCM 拷进 `data`，
   不足部分**补静音**并计入**欠载统计**；队列空则整块静音；
4. **音频时钟**：把 `getAudioRenderPosition()` 的数据源从"Java `AudioTrack.getPosition()`（JNI）"
   换成 `AAudioStream_getTimestamp(CLOCK_MONOTONIC)` 的 `presented` 帧数（取不到就沿用上次值）。
   我们的时钟模型（`目标点 + 设备已消费量`，`SuperMediaPlayer::getAudioPlayTimeStamp()`）**不需要改**，
   只换读数来源；
5. **flush/保活语义映射**（这是我们反复踩过的地方，必须逐条对齐）：
   - `flush`：`requestPause` → `requestFlush` → （必要时）`requestStart`，并把"已消费量快照"
     与 `AudioTrackRender` 里 `mAudioFlushPosition` 等价地重钉（否则位置会倒着拖 —— 这正是我们
     这轮在 `FlushAudioPath` 修过的坑）；
   - **保活**：AAudio 的 data callback 由平台按需拉取 ⇒ **不会出现"框架判 underrun 把 track 停掉"
     那类 OEM 行为**（`-1003`/`baseStop` 的来源），因此 `AudioTrackRender` 里那套"静音保活回合 +
     `AUDIO_KEEP_ALIVE_MAX_WRITES` 上界"在 AAudio 上**天然不需要**；回调里补静音即可；
   - `pause`：`requestPause`（设备暂停 ⇒ 已消费量不增长 ⇒ 位置恒定，与现有模型一致）；
6. **错误/设备切换**：`error_callback` → 通知上层**整体重载音频输出**（对齐 mpv `ao_request_reload`），
   不做计时器、不做局部兜底；
7. **构建**：`framework/render/CMakeLists.txt` 的 Android 段加源文件；两个模块
   `CMakeLists.txt` 的 `target_link_libraries` 加 `aaudio`（当前只链了 `OpenSLES`，且那是遗留）。

## 三、验收标记（真机）

- 启动日志：`[aaudio] output=aaudio … performanceMode=… rate=… channels=… bufferFrames=…`；
  日志出现 `[aaudio] libaaudio unavailable ⇒ fall back to AudioTrack` 时说明走到了回退（预期是
  API<26 或符号缺失）；
- 关键指标与现有 AudioTrack 路对比：`audio first frame after seek … afterSeekMs`、
  underrun/欠载计数、A-V 偏移是否更稳；设备位置读数不再出现"flush 后归零"导致的异常；
- **不得**出现：`onAudioException -1003`、`baseTimeout/baseStop`、`keep-alive: reached the resource cap`
  （AAudio 路结构上不会产生这几条）。

## 四、风险与回退

| 风险 | 对策 |
|---|---|
| 低延迟模式在个别设备上不可用/爆音 | `setPerformanceMode` 失败即退 `NONE`（实现里是"先试 LOW_LATENCY，openStream 失败就以 NONE 重开一次"，见 `AaudioRender.cpp` 的 openStream）；`setBufferCapacityInFrames` 用约 50ms 的期望值起步，设备会自行夹取 |
| 时间戳在个别设备上不可靠 | 与 mpv 同法：**取不到就返回 INT64_MIN**（上层 `SuperMediaPlayer::getAudioPlayTimeStamp()` 把它当"音频时钟暂不可用"退回自走时钟）；设备把 flush 后的帧计数归零时，用"presented < baseline 就地重钉基线"自愈 |
| 双实现（AAudio + AudioTrack）与工程偏好冲突 | 短期必要（minSdk 24）；**AAudio 在真机验证稳定后，把 minSdk 提到 26 并删除 AudioTrack 那份**，回到单一实现 |
| 回退判定错（明明支持却判定不支持） | `is_supported()` 只看"必需符号是否全部解析成功"，不做版本号判断（与 mpv 一致，最稳） |
| 注册顺序不确定（两个 .cpp 的静态对象构造顺序未定义） | **不依赖注册顺序**：`AudioTrackRender::is_supported()` 返回 `!AaudioRender::isAvailable()`，AAudio 可用时它自己让位 ⇒ 与顺序无关的"默认 AAudio、失败回退"。附带收益：这条调用使 `AaudioRender.o` 成为必需依赖，不会被静态库链接器丢掉 |

**回退**：`audioRenderPrototype` 的注册顺序把 AudioTrackRender 保留在 AAudio 之后；
如需整块撤销，删除新文件 + 注册行即可（不影响其它平台）。

## 五、构建接入的实测结论（2026-09-27）

已改：`framework/render/CMakeLists.txt` 的 Android 段（`audio/Android/AaudioRender.{cpp,h}`）。

**有意未改**：`platform/Android/ComposePlayer/cicadaplayer/src/main/cpp/CMakeLists.txt` 与
`platform/Android/source/premierlibrary/CMakeLists.txt` 的 `target_link_libraries` **没有加 `aaudio`**。
理由（两条，都指向"加了可能更糟"）：

1. **本实现不需要任何链接期符号**：不 include `<aaudio/AAudio.h>`、不调用任何 `AAudio*` 函数，
   全部符号都是运行时 `dlopen("libaaudio.so") + dlsym` 得到的。写成函数指针调用之后，
   链接期没有未定义符号需要 `-laaudio` 去补。
2. **`-laaudio` 在 minSdk 24 上有两个真实风险**（本工程 `minSdk = 24`：ComposePlayer 的
   `app/build.gradle.kts:54`、`cicadaplayer/build.gradle.kts:71`、`premierlibrary/build.gradle:16`；
   NDK `25.2.9519653`）：
   - **链接期**：AAudio 是 API 26 才有的库，API 24 的 sysroot 里不一定有它的 stub ⇒
     `cannot find -laaudio`，直接编不过；
   - **运行期更严重**：若链接器把 `libaaudio.so` 记成 DT_NEEDED，那么装到 **API 24/25** 设备上
     时动态链接器会因找不到该库而**让整个 `libCicadaPlayer.so` 加载失败** —— 那是启动即崩，
     比"这台设备没有 AAudio"严重得多。
   而 API ≥ 26 的设备上 `libaaudio.so` 是系统库，`dlopen("libaaudio.so")` 按 soname 一定能找到，
   **所以不加链接行不会让 AAudio 少用上**。

⇒ 结论：**保持不链接**。若将来把 minSdk 抬到 26（那时也该删掉 AudioTrack 那份），
或你确认 NDK 25 在 API 24 的 sysroot 里提供了 stub 且不会产生 DT_NEEDED，
就在那两处 `target_link_libraries` 里各加一行 `aaudio` 即可 —— 这一条需要**编译/真机验证**，见文末清单。

## 六、最终决策：高版本安卓用 AAudio、其余回退 AudioTrack，minSdk 保持 24

产品要求：**"支持 AAudio 的必须用 AAudio，不支持才回退 AudioTrack"** ——
新设备拿 AAudio 的性能，旧设备照样能正常播。落地方案：

| 项 | 决定 | 理由 |
|---|---|---|
| minSdk | **保持 24（Android 7.0）** | "支持就用 AAudio"是**运行期能力判定**，与安装门槛无关；抬 minSdk 只会缩小安装面（24 → 27 丢 1.6%，→ 28 丢 3.9%），换不到"更优先地用上 AAudio" |
| 用 AAudio 的门槛 | **API ≥ 27（Android 8.1）** | AAudio 核心符号是 `__INTRODUCED_IN(26)`，但 Google 的 Oboe 明确写着 "AAudio is not recommended for Android 8.0 or earlier versions" ⇒ "支持"按"推荐"算。门槛定义在 `AaudioRender.cpp` 的 `AAUDIO_MIN_RECOMMENDED_API`，**只有这一处**，要挪到 26/28 改一个常量 |
| 26 与更早 | **AudioTrack** | 8.0 的 AAudio 在但不推荐；7.x 根本没有 `libaaudio.so` |
| "存在但打不开" | **sticky 自降级到 AudioTrack** | 符号齐全不等于能用；真实建流失败后置进程级标记，后续 `create()` 直接选 AudioTrack（无计时器、无重试循环） |
| 建流失败时 | 先做 **3 轮**降级尝试（低延迟+期望缓冲 → 普通+期望缓冲 → 普通+不指定缓冲），3 轮都失败才认"这台设备用不了 AAudio" | 把"能建起来"的概率做满，减少真正需要回退的情形 |
| "存在但打不开"如何**当场**回退 | `mediaPlayer/SMPAVDeviceManager.cpp` 的 `setUpAudioRender()`：`init()` 失败 ⇒ 销毁该实例 → `AudioRenderFactory::create()` 再建一个 → 再 `init()` 一次（**最多 1 次重试**；纯状态判据，无计时器） | 失败方已经把自己标成不可用（sticky），所以第二次 `create()` 必然选到 AudioTrack ⇒ **第一次音频 setup 就能回退**，而不是让整次播放报 `MEDIA_PLAYER_ERROR_RENDER_AUDIO_OPEN_DEVICE_FAILED` |

### 版本支持矩阵（minSdk 24 不变）

| Android | API | 走哪条路 | 说明 |
|---|---|---|---|
| 5.0 ~ 6.0 | 21 ~ 23 | 不覆盖 | 低于 minSdk 24 |
| 7.0 / 7.1 | 24 / 25 | AudioTrack | 无 `libaaudio.so` |
| 8.0 | 26 | **AudioTrack** | AAudio 在、符号也齐，但 Google 不推荐（"not recommended for Android 8.0 or earlier"） |
| 8.1 | 27 | **AAudio** | 推荐门槛（Oboe 同值） |
| 9 ~ 16 | 28 ~ 36 | **AAudio** | `setUsage` 等齐全（API 28），行为最稳 |
| 12+ | 31+ | AAudio | 只多一个本实现未使用的 `setChannelMask`(32) |

覆盖率（Google 2025-12 分布）：minSdk 24 ⇒ 可装 **99.1%**；其中走 AAudio 的 ≥27 段为 **97.5%**。

### 这一步新增的验收标记（真机一眼判定）

- 用上 AAudio：`[aaudio] output=aaudio stream opened: performanceMode=… rate=… channels=… bufferCapacityFrames=… ringBytes=…`
- 因系统版本回退：`[aaudio] not used on this device (API level=26 < 27) ⇒ fall back to AudioTrack …`
- 因符号缺失回退：`[aaudio] libaaudio unavailable ⇒ fall back to AudioTrack …`
- 因"打不开"自降级：`[aaudio] openStream failed (attempt N/3 …)` 之后
  `[aaudio] cannot open an output stream after 3 attempts … AAudio disabled for the rest of this process`
  （下一次音频 setup 会是 AudioTrack）


