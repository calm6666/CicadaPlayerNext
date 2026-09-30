# macOS 支持补完 · 差距清单与实施计划

基准 = Windows/D3D11 那条路。本文件是"只读差距审计"的结论沉淀（审计只读代码、未改文件），
用于把 macOS 对齐工作拆成可验收的步骤。**Apple 侧无法在 Windows 本机编译验证**，
所有"已实现/部分"在 macOS 真机跑过之前都只是读代码推断。

## 1. 现状（要点）

| 功能 | Windows | macOS |
| --- | --- | --- |
| 零拷贝直通 | D3D11 视频处理器 NV12/P010→RGBA | 骨架完整（`CicadaTextureMetal.mm` + 门面 + CMake `:436-442`/`:898-913`），**未真机验证** |
| 硬解开关/运行期降级 | 门面 + item | 同一门面；降级后画面冻结（G1/G2） |
| 色彩调整 b/c/s | `CicadaTextureD3D11` 处理器过滤器 | **缺失**（`CicadaVideoTexture.cpp` 硬编码 `#if Q_OS_WIN`） |
| HDR/P010/色彩空间 | DXGI_COLOR_SPACE + P010 | **缺失**（强制 32BGRA 绕过了 10bit 分支） |
| 旋转/镜像/缩放、QML 属性、缩略图、原生文件对话框 | — | 平台无关，已实现（未验证） |
| 截图/快照 | 完整 | **实现但不可达 = 等于缺失**（G1） |
| CPU 回退 | 完整 | **实现但拿不到帧**（G2） |
| 释放一族 | `releaseInputView`… 全套 | `releaseInputState` 已转发到 Metal（本轮补）；`flushDeferredDestruction`/Trim 无对应物（可接受） |
| 内存/句柄/线程探针、看门狗 | Win32 | **缺失**（G5） |
| 打包/签名 | DLL install | 部分：缺框架 dylib、部署目标 10.11 过旧、无签名（G8） |

## 2. 缺口清单与状态

| # | 内容 | 状态 |
| --- | --- | --- |
| **G1** | macOS 截图永远空图：`CicadaVideoRender.cpp` 的 3b（`isGpuOnlyPixelFormat`）把 `AF_PIX_FMT_APPLE_PIXEL_BUFFER` 拦在 3c 的 Apple 分支**之前**，那段分支是死代码 | **本轮已修**（用 `#if defined(__APPLE__)` 把该枚举从 3b 排除，判定落到 3c；只影响 Apple，Windows 判定逐字不变） |
| **G2** | `PBAFFrame::operator AVAFFrame*()` 只认 NV12/YUV420P，而播放器强制 VT 输出 32BGRA ⇒ CPU 回退/截图都拿不到帧 | **本轮已修**（加 `32BGRA → AV_PIX_FMT_BGRA`，并把它的 colorRange 记为 FULL；非平面格式走 `GetBaseAddress`/`GetBytesPerRow` + `av_image_copy`） |
| **G3** | Metal 色彩调整未实现（需自有输出纹理 + `CIFilter CIColorControls` 或 Metal 着色器） | 待做（Step 4） |
| **G4** | HDR/P010 与色彩空间被 32BGRA 绕过（`VTPixelTransferSession` 显式色调映射） | 待做（Step 5） |
| **G5** | macOS 无内存/句柄/线程探针，看门狗只在 `#ifdef Q_OS_WIN` 内（`task_info`/`TASK_VM_INFO`、`task_threads`、`proc_pidinfo`、`backtrace`） | 待做（Step 6） |
| **G6** | macOS 显卡名永远为空（`MTLCreateSystemDefaultDevice().name` 或 IORegistry） | 待做（Step 6） |
| **G7** | Metal 每帧新建 QSGTexture 包装、无世代缓存（`CicadaTextureD3D11` 有 `m_outputQsTexture`/generation 可对齐） | 待做（Step 7；G3 引入自有输出纹理后天然成立） |
| **G8** | 打包：框架 dylib 不随 bundle、`MACOSX_DEPLOYMENT_TARGET 10.11` 与 Qt6 冲突、无签名 | 待做（Step 7） |
| **G9** | Metal 无 `trimVideoMemory()` 等价物（`CVMetalTextureCacheFlush` 已覆盖主要面） | 可选 |

## 3. 实施顺序与验收

- **Step 0**：确认 macOS 上 `Qt6::GuiPrivate`/`Qt6QuickPrivate`/`ENABLE_VTB_DECODER` 齐备，`./build_macos.sh` 能起 `.app`。
  验收日志：`Qt scene graph graphics API: Metal (RHI)`、`Metal zero-copy is ready: ...`、`first zero-copy frame: WxH BGRA CVPixelBuffer -> MTLTexture`。
- **Step 1（G2，本轮已改）**：`QSG_RHI_BACKEND=opengl` 强制 CPU 路径起播。
  验收：有画面；不再出现 `cannot download the CVPixelBuffer frame for the CPU path`。
- **Step 2（G1，本轮已改）**：暂停后拖进度条 / `requestSnapshot()`。
  验收：出现 `captureScreen: delivering a WxH RGBA snapshot`，`image://snapshot/<rev>` 出图且颜色与画面一致。
- **Step 3**：`QSG_RHI_BACKEND=metal` 播 1080p H.264/HEVC 各 10 分钟。
  验收：`zeroCopy` 为 true；无 `zero-copy failed at runtime, switching to the CPU path`；每次切档各打一次 `[mem] releaseInputState: flushed the Metal texture cache ...`。
- **Step 4（G3）**：先 `CIFilter CIColorControls`，不够再换 Metal 着色器。
  验收：`colorAdjustSupported` 为 true（面板不再显示"仅界面预览"）；brightness 50/150 亮度单调变化且 `zeroCopy` 仍为 true。
- **Step 5（G4）**：`VTPixelTransferSession` 显式色调映射（先保 BGRA 路，双平面 10bit 直通列为后续）。
- **Step 6（G5+G6）**：mach 版 `[mem]` 读数（footprint/线程/fd）+ 显卡名。
- **Step 7（G7+G8）**：包装世代缓存 + bundle dylib/rpath/部署目标/签名。

## 4. 风险

1. 本机（Windows + MSVC 静态 Qt）**编不了 `.mm`**，也跑不了 Metal/VideoToolbox/AudioQueue/QPA 对话框/bundle；只能保证"Apple 代码全部关在 `#if defined(Q_OS_MACOS)`/`__APPLE__` 与 CMake `APPLE` 分支内，且 Windows 构建零错误零警告"。
2. `PBAFFrame.cpp` 只在 Apple 编译（`framework/utils/CMakeLists.txt`），本轮改动**无法在本机编译验证**，为评审级。
3. Qt 6.5~6.7 走 `wrapNativeTextureWithRhi`（需 `Qt6::GuiPrivate`），Metal 的 `layout=0` 兜底从未真机验证 —— 若用 6.5~6.7 编 macOS，这是第一优先要验的组合。
4. `CicadaTextureMetal.mm` 的 3 槽帧环深度是估计值；高刷屏 + 弹幕 60Hz 重绘若出现偶发花屏，先加大槽数。
5. `framework/macOSX.cmake` 的 `MACOSX_DEPLOYMENT_TARGET 10.11` 低于 Qt6/Metal 实际要求，建议 Step 0 一并处理。
