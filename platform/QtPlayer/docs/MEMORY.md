# 内存占用：实测数字、构成、以及"换片不回收"那个坑

> 起因：用户反馈"播放一个视频内存直接过一百多，甚至三四百，这不是一个简单的 QML 程序该有的占用"。
> 下面每个数字都是**实测**的（`Get-Process` 采 WorkingSet / PrivateMemorySize，机器上直接跑的 Release 版），
> 不是估的。测量用的片子是用 ffmpeg 现生成的 1080p/30s 和 4K/8s（`testsrc2`）。

## 一、实测（Release / MSVC / D3D11 零拷贝）

| 状态 | WorkingSet | Private | Handles |
|---|---|---|---|
| 空载（没选片子） | 94.8 MB | 116 MB | 974 |
| 放 1080p（H.264 8bit） | 峰值 177 MB → 收在 ~130 MB | 峰值 209 MB → ~160 MB | ~1030 |
| 放 4K（H.264 8bit） | 峰值 218 MB | 峰值 295 MB | ~1030 |

结论：**空载就已经 95MB/116MB**，播放再加几十到两百多 MB。所以"过一百多"里，
大头是空载基线 + 解码器表面池，**不是 QML 那层涨上去的**（QML 对象总数是常数：面板/菜单/弹幕池都是固定大小）。

## 二、那 60MB / 250MB 是什么：FFmpeg 的 D3D11VA 表面池

硬解（D3D11VA）在**第一帧**就会一次性申请一整张表面纹理数组：

* 池子大小 = `initial_pool_size`，d3d11va 里是 `1 + 16`（`libavcodec/dxva2.c:619-634`），
  decode.c 还会再加 3 个（`decode.c:1108`）→ 一共 **20 个 surface**；
* 这 20 个 surface 不是 20 张纹理，而是**同一个 `ID3D11Texture2D` 的 ArraySize=20**
  （`libavutil/hwcontext_d3d11va.c:299-330`）；
* 所以一次申请就是：1080p NV12 ≈ **62MB**、4K NV12 ≈ **251MB**、4K P010(10bit) ≈ **501MB**。

关键点：**每个 surface 的 AVBufferRef 都对整张数组 AddRef**（`hwcontext_d3d11va.c:251-270`），
所以"还有一帧活着"就等于"整池还活着"。而这条路**关掉硬解（copy-back）也一样要付**，
因为池子是在解码器 Prepare/第一帧时按容器能力申请的，不由我们的开关决定。
这是 FFmpeg 内部行为，应用侧没有能把 20 个降下来的选项。

## 三、修掉的那个真问题：换片源时上一池不回收

`CicadaPlayerItem::destroyPlayer()` 原来释放了 player / listener / pendingFrame，
但**没放渲染线程手里的两个引用**：

* `m_currentFrame`（只在渲染线程读写，见 `updatePaintNode`）；
* `CicadaTextureD3D11::m_inputView`（视频处理器的输入视图，对解码纹理持有 D3D11 引用）。

因为上面那条"一个引用钉住整池"，换片源/停播之后上一个解码器的整池显存（4K 约 240MB）
会一直被留着 —— 现象就是"放过几条片子之后内存下不来"。

### 改法（4 个文件，跨线程按"标志 + update()"的既有套路）

| 文件 | 改动 |
|---|---|
| `src/CicadaPlayerItem.h` | 新增 `std::atomic<bool> m_releaseRenderState{false}` |
| `src/CicadaPlayerItem.cpp` | `destroyPlayer()` 里置位并 `update()`（**放在 `m_player == nullptr` 提前返回之前**）；`updatePaintNode()` 开头 `exchange(false)` 取走，`m_currentFrame.reset()` + `m_textureBackend->releaseInputState()` |
| `src/CicadaTextureD3D11.h/.cpp` | 新增 `releaseInputView()`：只放输入视图并清掉它缓存的解码纹理指针/切片号；设备、处理器、枚举器、输出纹理**全部保留**（它们跟片源无关，重建很贵） |
| `src/CicadaVideoTexture.h/.cpp` | 新增 `releaseInputState()`：Windows 转发给 D3D11 后端；macOS/Linux 是**有意的空实现**（那两个后端本轮没实测过，不能凭空写释放动作） |

三个"不要动"（试过会坏）：

1. **别**在 `destroyPlayer()` 里调 `releaseResources()`：它会把 `m_prepared`/`m_zeroCopyActive`
   清成 false，而 `Prepare()` 之前没人重新探测 → `startPlaybackWhenReady()` 会读到错的探测结果、
   把进程级的 `video.render.hw.direct_texture` 写成 "OFF"，画面直接冻住。
2. **别**想缩小解码池：20 个 surface 是 FFmpeg 内部定的，应用侧改不了。
3. **别**去掉 `avcodecDecoder::flush_decoder` 里的 `av_frame_unref` 循环：那是防"seek 之后
   把旧帧交给渲染器"，不是泄漏。

已知副作用（可接受）：放掉之后如果这一刻还没有新帧，画面区会立刻空掉（而不是把上一条片子的
最后一帧留在屏幕上）—— 换片源时本来就该这样，而且只在**换片源/停播**时发生，正常播放不受影响。

## 五、这台机器实测走的是哪条路（零拷贝开着）

把日志接下来看（**必须正常关窗口**，直接 kill 会丢掉 printf 的缓冲区）：

```
appQtPlayer.exe D:\some\video.mp4 > log.txt 2>&1      # 然后正常关闭窗口，再看 log.txt
```

实测（GTX 1650 Ti / Qt 6.11.1 / Release）：

```
captured Qt's D3D11 device for zero-copy decoding: Qt D3D11 device (NVIDIA GeForce GTX 1650 Ti)
D3D11 zero-copy is ready: decoded NV12/P010 textures are converted to RGBA by the D3D11 video processor
zero-copy direct output, decoded textures go straight to the presenter (no download, no CPU copy)
first zero-copy frame: 1920x1080 NV12 texture -> RGBA on the GPU -> Qt
```

也就是说：**零拷贝是开着的**，那张 20 片的表面数组在**显存**里（任务管理器的"专用 GPU 内存"），
不占系统内存。所以本项目在系统内存上的构成就是：

| 情况 | 系统内存（Private） | 显存 |
|---|---|---|
| 空载 | 116 MB | 很少 |
| 1080p 零拷贝 | 基线 + 输出纹理(8.3MB) + 一点 → 实测峰值 209MB | +62 MB（20 片池） |
| 4K 零拷贝 | 基线 + 输出纹理(33MB) + 一点 → 实测峰值 295MB | +251 MB（20 片池） |

**怎么判断自己落在哪种情况**（不改代码，看界面上的诊断文字 / 日志）：
* `player.backend` / `player.zeroCopy` / `player.device`（Main.qml 的诊断浮层）；
* 日志里那句 `D3D11 zero-copy is ready` = 零拷贝开着；出现
  `Qt's D3D11 device has no video support … (copy-back)` = 退回 copy-back，
  这时**显存池和系统内存要同时付**：下载池 6 槽（1080p 17.8MB / 4K 71.7MB）+ 转换帧（3/12MB）；
  再退回 CPU 兜底还要加 QImage + 上传纹理（1080p 7.9+7.9MB / 4K 31.6+31.6MB）。
* 只有**纯软解**才会单靠自己到 300–400MB 系统内存（FFmpeg 的 DPB 200–250MB@4K）。

## 六、换片 / 关窗之后内存会不会降下来（**已实测**）

以前这条写着"需要手动换片、没有实测"。现在有了：用一段临时脚本（`createObject(Main.qml)` →
`playFile(A)` → 播 8 秒 → `close()` → `deleteLater()` → 再开一个窗口 → `playFile(B)`，A/B 交替
共 4 个窗口，验完已删）跑 **1080p 零拷贝**，同时每 1.5 秒采一次进程内存：

| 时刻 | WS | Priv |
|---|---|---|
| 只有首页窗口（基线） | 17 MB | 7 MB |
| 第 1 个播放器窗口播放中 | 158→161 MB | 210→211 MB |
| 关掉之后 | 133 → 118 MB | 178 → 162 MB |
| 第 2 个窗口播放中 | 179-180 MB | 223 MB |
| 关掉之后 | 157 → 127 MB | 194 → 164 MB |
| 第 3 个窗口播放中 | 177-179 MB | 219 MB |
| 关掉之后 | 163 → 130 MB | 198 → 165 MB |
| 第 4 个窗口播放中 | 176-179 MB | 214-229 MB |
| 关掉之后 | 156 → 129 → 125 MB | 194 → 166 → 162 MB |
| 全部关掉、脚本结束 | 97 MB | 74 MB |

**结论：反复开关不涨**。每一轮的峰值都在同一水平（WS ~177-180MB / Priv ~215-229MB），
关掉后都回落到同一水平（WS ~125-140MB / Priv ~162-178MB）——峰值和地板都不随轮次抬升，
所以这条路径**没有泄漏**。地板比基线高是正常的：首页窗口自己占着 + 系统堆不会立刻还给系统
（不是泄漏，因为不随轮次增长）。

**谁保证了"关掉就还"**（按释放顺序）：
1. `Main.onClosing` → `player.stop()` + `playerView.source = ""` → `CicadaPlayerItem::setSource("")`
   → `destroyPlayer()`：置 `m_releaseRenderState`（渲染线程下一帧放掉**上一帧 + D3D11 输入视图**，
   这是钉住整池表面纹理的那两个引用）、`Stop()`、销毁 `MediaPlayer`（解码器 + 表面池）、清挂起帧；
2. 窗口销毁/场景图回收 → `onSceneGraphInvalidated()`：纹理后端 `releaseResources()`
   （输出纹理/处理器/枚举器/视图/视频接口全 Release）+ `CicadaHardwareDevice::releaseFromSceneGraph()`
   （把借给 FFmpeg 的那份 Qt 设备**还掉**，见下）；
3. `~CicadaPlayerItem`：再兜一遍 `destroyPlayer()` + `releaseResources()`（都是幂等的）。

**关于那个"借来的设备"**：`CicadaHardwareDevice` 是进程级单例，缓存的 `ID3D11Device` 现在
**由它自己持有一份 COM 引用**（拿到 AddRef、场景图回收时 Release）。以前它只是"借用 + 不持有"，
于是窗口销毁后缓存里会留一个已被 Qt 释放的指针 —— 这既是泄漏（那份引用永远不会还），
也是"关掉播放窗口再开新视频就闪退/卡在正在加载"的根因，详见 `HANDOVER-TODO.md` 第三节 4c。

## 七、还没实测到的部分（说清楚）

* 4K 素材上的"反复开关"没量过（上面那组只用了 1080p 的两条相机视频）；
* macOS / Linux 那两条后端（Metal / VAAPI）的"停播后显存回收"没实测过
  （`releaseInputState()` 在那两个平台是**有意的空实现**，见 `CicadaVideoTexture.cpp` 里的说明）。
