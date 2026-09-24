# CicadaPlayer Qt6 QML 播放器组件

把 CicadaPlayerNext 的媒体能力（demux / 解码 / 音视频同步）封装成一个 **QML 元素**，
视频画面直接进 Qt 的场景图 —— **绘制全部由 Qt 完成**（QSGTexture + QSGNode + RHI 后端），
本组件里没有一行自己管理 OpenGL/D3D 状态机的代码；在能拿到解码纹理的平台上是
**硬件解码 + 零拷贝**上屏。

三端都支持：Windows / Linux / macOS。

```qml
import QtPlayer

CicadaPlayerItem {
    anchors.fill: parent
    source: "file:///D:/video/4k.mp4"
    autoPlay: true

    onFirstFrameRendered: loadingAnimation.stop()
}
```

换片子不用改代码，也不用重新编译（三条路都落到同一个 `playFile()` 上）：

| 方式 | 怎么做 |
|---|---|
| 文件对话框 | 点左下角「打开」按钮（`QtQuick.Dialogs`，Windows 上是系统原生对话框） |
| 拖拽 | 把视频文件直接拖进窗口 |
| 命令行 | `appQtPlayer.exe D:\video\4k.mp4`（网络地址同样可以：`https://host/a.m3u8`） |

演示界面的控制条（`Main.qml`，全部手画，不依赖 QtQuick.Controls）：

| 控件 | 作用 |
|---|---|
| 「打开」按钮 | 系统文件对话框选本地视频 |
| 播放 / 暂停按钮 | `player.togglePause()` |
| 进度条 | 点击或拖动定位 → `player.seek()`。**可以放心拖**：组件内部做了"单飞 + 最新优先"，框架同时只会看到一个 seek（见 §5 的说明） |
| 音量滑条 | 点击或拖动调音量 → `player.volume`（0.0 ~ 1.0） |
| 「设置」按钮 | 打开设置页（见下） |
| 全屏按钮 | `Window.visibility` 在 `FullScreen` / `Windowed` 之间切；全屏后再点一次或按 **Esc** 还原 |
| 标题栏（顶部那条） | 无边框窗口自己画的标题栏：左边标题，右边主题（太阳/月亮）+ 最小化 / 最大化 / 关闭（见 §3.6）。拖动它移动窗口、双击它最大化、右键弹系统菜单，都是 QWindowKit 走原生实现的 |
| 太阳 / 月亮按钮 | 深↔浅一键切换（见 §3.7） |
| 左上角文字 | 零拷贝状态 + 后端名 / 文件名 / 解码设备 / 视频尺寸 + 状态 / 错误（可在设置页里关掉） |

设置页（遮罩 + 面板，点面板外或按 Esc 关闭）：

| 选项 | 对应 | 说明 |
|---|---|---|
| 界面主题 | `QtPlayerTheme.mode` | 跟随系统 / 浅色 / 深色（见 §3.7）。默认跟随系统 |
| 硬件解码（硬解） | `player.hardwareDecoding` | 关掉即软件解码，画面自动走 CPU 回退路径。**改它会重新打开当前视频并 seek 回原位置**（解码器是在 `Prepare()` 里建的，改完必须重来一遍） |
| 循环播放 / 静音 / 水平镜像 | `player.loop` / `muted` / `mirror` | 组件本来就有这些属性 |
| 播放倍速 | `player.playbackRate` | 0.5x / 1x / 1.5x / 2x（`MediaPlayer::SetSpeed`） |
| 画面旋转 | `player.extraRotation` | 0/90/180/270，和视频自带旋转元数据叠加 |
| 显示诊断信息 | `root.showDiagnostics` | 关掉左上角那几行 |

> 控制条**自动隐藏**：鼠标动一下就淡入，停 3 秒淡出；鼠标压在控制条上、或者设置页 /
> 文件对话框开着时不藏。隐藏时 `bar.visible` 跟着变假，所以藏起来之后不会有一排看不见的
> 按钮吃鼠标点击。控制条藏起来时**鼠标指针也一起隐藏**（`cursorShape: Qt.BlankCursor`），
> 鼠标一动控制条和指针一起回来。
>
> 设置只在本次运行内有效（没有落盘）：要持久化的话，`QtCore.Settings`（QSettings）加几行就行，
> 这里刻意没引入，免得组件多一份"隐式状态"。

> 这些函数（`playFile`/`sourceName`/`formatTime`/`seekAt`/`toggleFullscreen`/`setVolumeAt`）
> 都声明在 QML 的**根对象**上：QML 的静态作用域只把根对象的函数带进整个组件，放到非根
> 对象里子对象的绑定会找不到（`ReferenceError`），按钮和滑条就会静默失灵。

---

## 1. 它是怎么工作的

```
 FFmpeg(硬解)            框架                        Qt 场景图
 ┌──────────────┐   ┌────────────────────┐   ┌──────────────────────────┐
 │ D3D11VA /    │   │ avcodecDecoder     │   │ CicadaPlayerItem         │
 │ VideoToolbox │──▶│ (不下载，直出纹理) │──▶│ updatePaintNode()        │
 │ VAAPI        │   │ CicadaVideoRender  │   │  └─ QSGTexture + QSGNode │
 └──────────────┘   │ (注入的"无窗口"渲染)│   │      ↓                   │
                    └────────────────────┘   │   QQuickWindow (RHI)     │
                                             └──────────────────────────┘
```

三个角色：

| 角色 | 文件 | 干什么 |
|---|---|---|
| 播放器组件 | `src/CicadaPlayerItem.h/.cpp` | QML 元素：属性、播放控制、帧的跨线程交接、等比适配 |
| 无窗口渲染器 | `src/CicadaVideoRender.h/.cpp` | 注册给框架的 `IVideoRender`：复用框架的 VSync 线程与帧节拍，把帧转交给组件。**一行图形代码都没有** |
| 纹理后端 | `src/CicadaVideoTexture.*` + `src/CicadaTexture*.{cpp,mm}` | 按平台把解码纹理变成 `QSGTexture`（零拷贝），做不到就退到 CPU 路径 |
| 纹理包装层 | `src/CicadaQtTextureWrap.h/.cpp/.mm` | 把**原生 GPU 纹理**包成 `QSGTexture`：Qt 6.8+ 走 `QNativeInterface::QSG*Texture::fromNative()`，6.5~6.7 走 `QRhiTexture::createFrom()` 兜底（Qt 6 删掉了 Qt 5 的 `createTextureFromNativeObject`），见 §2.1 |
| 硬件设备 | `src/CicadaHardwareDevice.h/.cpp` | 把 Qt 的 GPU 设备交给 FFmpeg；Windows 上还能按需自建带 `VIDEO_SUPPORT` 的 D3D11 设备交给 Qt，见 §2.2 |

### 线程模型（最重要的一点）

| 线程 | 做什么 | 绝对不能做什么 |
|---|---|---|
| 框架 VSync 线程 | `CicadaPlayerItem::handleVideoFrame()`：`clone()` 一帧 + **队列投递**一次 `update()` | 碰任何 Qt 图形 API（QSGTexture / RHI / 原生设备句柄）；直接调 `QQuickItem::update()`（Qt 会拒绝并打日志，画面就不重绘了） |
| Qt 渲染线程 | `updatePaintNode()`：帧 → `QSGTexture` → 场景图节点 | 阻塞、加长锁、调用播放器的同步接口 |
| Qt 主线程 | 播放控制、属性更新、监听回调 | —— |

`clone()` 是必须的：回调返回后框架就会回收那一帧，而渲染线程可能还在用它；
副本保证解码器的表面池不会在画面用之前被复用。

---

## 2. 零拷贝怎么实现的

| 平台 | 硬解 | 零拷贝路径 | 说明 |
|---|---|---|---|
| **Windows** | FFmpeg D3D11VA | 解码纹理 → **D3D11 视频处理器**(NV12/P010→**RGBA**，GPU) → `QSGD3D11Texture::fromNative()` / `QRhiTexture::createFrom()` | 关键是让 FFmpeg 用 **Qt 场景图的那个 `ID3D11Device`**，否则纹理没法给 Qt 采样（见 §2.2）。输出纹理必须是 `DXGI_FORMAT_R8G8B8A8_UNORM`：`QSGTexture` 没有像素格式参数，Qt 只会按 RGBA8 建 SRV，格式不一致就是 `E_INVALIDARG`（画面全黑） |
| **macOS** | 框架自研 `AFVTBDecoder`（VideoToolbox） | CVPixelBuffer(BGRA) → `CVMetalTextureCache`（IOSurface 直通）→ `QSGMetalTexture::fromNative()` | 框架的 VTB 解码器**本来就直出 CVPixelBuffer**（不下载）；只要把 `pixelBufferOutputFormat` 设成 `32BGRA`，Qt 侧就是一条干净的零拷贝路 |
| **Linux** | FFmpeg VAAPI | VASurface → **VAAPI VPP**(NV12→BGRA，GPU) → dmabuf → `eglCreateImageKHR` → GL 纹理 → `QSGOpenGLTexture::fromNative()` | 要求场景图跑在 **OpenGL** 上；转成单平面 BGRA 之后 dmabuf 能直接绑成 `GL_TEXTURE_2D`，Qt 默认材质就能采样（不需要自定义着色器和 `samplerExternalOES`） |
| 任意平台 | 软解 / 上面都不满足 | swscale → `QImage` → `QSGTexture::createFromImage` | **CPU 回退**：慢（4K 每帧约 50MB 内存搬运），但一定播得出来 |

> 前两条路上，NV12→RGBA / BGRA 直出都是**解码器或 GPU 内部**完成的转换，不是 CPU 转换；
> 零拷贝的含义是"没有任何 CPU 拷贝、没有 GPU↔内存往返"。Qt 侧只负责采样和缩放。

### 2.1 原生纹理怎么交给 Qt（Qt 6 和 Qt 5 不一样）

Qt 5 有 `QQuickWindow::createTextureFromNativeObject()`，**Qt 6 删掉了**。Qt 6 里的两条官方路子
都封在 `src/CicadaQtTextureWrap.h/.cpp(.mm)` 里，三个平台后端只调 `wrapD3D11Texture()` /
`wrapMetalTexture()` / `wrapGLTexture()`：

| Qt 版本 | 用什么 | 备注 |
|---|---|---|
| **6.8+** | `QNativeInterface::QSG{D3D11,Metal,OpenGL}Texture::fromNative(...)` | 首选。Qt 自己建 `QSGTexture` 包装，内部处理 SRV / 纹理目标等细节 |
| **6.5 ~ 6.7** | `QRhiTexture::createFrom(NativeTexture)` + `QQuickWindow::createTextureFromRhiTexture()` | 兜底。这两个接口从 Qt 6.0 就有，所以 6.5 也能编能跑。注意 `rhi/qrhi.h` 在 Qt 的**版本化头目录**里，CMake 会为这种情况自动链上 `Qt6::GuiPrivate`（探测不到时源码用 `__has_include` 自动编成"没有这条路线"，仍然能编能跑 CPU 路径） |

两条路都**不需要我们自己写 OpenGL/着色器**：采样、材质、批处理全部由 Qt 场景图完成，
我们只把解码出来的纹理递进去（`QQuickWindow::TextureOwnsGLTexture` 也没用 —— 纹理是我们
复用的双缓冲，生命周期必须自己管）。

### 2.2 Windows：解码和 Qt 采样要落在同一个 D3D11 设备上

零拷贝的前提是"解码纹理"和"Qt 采样的纹理"在**同一个** `ID3D11Device` 上（D3D11 里两个不同
设备的纹理不能互相采样）。做法是把 Qt 场景图正在用的设备借给 FFmpeg 解码（框架侧新增的
`HwDeviceBridge` 钩子），但这里有个硬性条件：

> FFmpeg 的 `d3d11va_device_init()`（`libavutil/hwcontext_d3d11va.c:508`）在使用外部设备时会
> `QueryInterface` 出 `ID3D11VideoDevice` / `ID3D11VideoContext`，**拿不到就直接失败**；而 D3D11
> 只在创建设备时带了 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` 才保证提供这两个接口（FFmpeg 自己
> 建设备时就是这么建的，同文件 `:626`）。

**图形设备一律用 Qt 选的，本工程不创建、不替换、不指定任何 GPU 设备**：用哪块卡、哪个 RHI
后端、混合显卡/软渲染怎么回退、设备丢了怎么重建，这些都是 Qt 处理得最稳的部分，插手只会更糟。
我们只做一件事 —— 在渲染线程上**读**一下 Qt 当前的设备，然后问它一句"有没有视频接口"，
据此选路（`src/CicadaHardwareDevice.cpp`，日志会讲清楚走的是哪条）：

* **有** → 把 Qt 的设备借给 FFmpeg 解码。解码、视频处理器、Qt 采样全在同一个设备上 → **零拷贝**；
* **没有** → **一个字节都不改 Qt 的设备**，只是不注册那个桥接：FFmpeg 照它一贯的方式自己建
  一个带 `VIDEO_SUPPORT` 的设备，**硬解仍然在 GPU 上**，帧下载回内存（copy-back），Qt 侧走
  CPU 上传路径。能播、能硬解，只是没有零拷贝。

真要想在"没有视频接口"的机器上也要零拷贝，就得让 Qt 用别的设备（`QQuickWindow::setGraphicsDevice`），
那等于替 Qt 决定 GPU 选型，本工程刻意不做。

### 怎么确认到底走了哪条路

看两处：

1. **界面左上角的诊断文字**（`Main.qml` 里绑的）：`零拷贝: 是/否` + 具体后端名 + 解码设备；
2. **日志**（框架自带日志）：

```
D3D11VA: hardware decoding enabled for hevc (hw pixel format d3d11)
D3D11VA: zero-copy direct output, decoded textures go straight to the presenter (no download, no CPU copy)
captured Qt's D3D11 device for zero-copy decoding: Qt D3D11 device (NVIDIA GeForce ...)
D3D11 zero-copy is ready: decoded NV12/P010 textures are converted to RGBA by the D3D11 video processor ...
first zero-copy frame: 3840x2160 NV12 texture -> RGBA on the GPU -> Qt
```

出现下面任意一条，说明退到了 CPU 路径（功能正常，只是费内存带宽）：

```
Qt's D3D11 device has no ID3D11VideoDevice ...           <- Qt 的设备没开视频支持 → 走 copy-back（硬解还在）
Qt scene graph is not using D3D11 ...                    <- 被 QSG_RHI_BACKEND 改了后端
zero-copy failed at runtime, switching to the CPU path ...
```

---

## 3. 构建

### 3.1 依赖

* **Qt 6.5 及以上**（Quick 模块；组件只用了 6.5 就有的公共 API）
* 本仓库框架的常规依赖（FFmpeg、curl、openssl、xml2……与 cmdline 完全相同）
* **SDL2**：只为**音频**（Windows/Linux 上框架的音频渲染器用它；macOS 用系统
  AudioQueue，不需要 SDL）。视频渲染器已被本组件注入的实现顶掉，不会建 SDL 窗口。
* Windows：需要 FFmpeg 构建时带 `d3d11va`（`external/player_ffmpeg_config.sh` 里的
  `--enable-hwaccels --enable-d3d11va`）。

### 3.2 编译（Linux / macOS 为例）

```bash
cd CicadaPlayerNext/platform/QtPlayer
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/appQtPlayer
```

Windows 上按本仓库既有方式用 vcpkg 工具链配置（和 `cmdline` 一致），把
`libffmpeg.dll` 放到 exe 旁边。

**vcpkg 必须显式给**（Qt Creator 默认的 kit 不带工具链，框架在 Windows/MSVC 上要
pthreads / sdl2 / libxml2，全都在 vcpkg 里）。工程里做了三件事：尊重你已有的
`CMAKE_TOOLCHAIN_FILE`；没给就读 `VCPKG_ROOT` / `VCPKG_INSTALLATION_ROOT` 环境变量
（不在这台机器上乱猜路径）；都没有就在配置阶段直接把要加的参数写在报错里。所以最省事的
做法是设一次环境变量：

```bat
setx VCPKG_ROOT "D:\Dev\vcpkg"     :: 换成你自己的 vcpkg 目录，然后重开终端/Qt Creator
```

或者把下面两条加到 Qt Creator kit 的 CMake configuration 里（Projects → Build Settings）：

```
CMAKE_TOOLCHAIN_FILE:FILEPATH=D:/Dev/vcpkg/scripts/buildsystems/vcpkg.cmake
VCPKG_TARGET_TRIPLET:STRING=x64-windows      # 32 位构建用 x86-windows
```

> **Windows 上有一条命令的构建脚本**（配置 + 编译 + 部署 + 依赖自检一条龙，
> 而且静态 / 动态 Qt 用参数切换）：见下面 **§3.8**。

### 3.3 控制台窗口：Debug 留着，Release 关掉

发布版双击不该弹黑框，但调试时又必须看得到框架的日志（`printf` 到 stdout，见
`frame_work_log.c`）。所以按配置区分：

| 配置 | 子系统 | 效果 |
|---|---|---|
| Debug | `/SUBSYSTEM:CONSOLE` | 有控制台窗口，日志直接可见 |
| Release / RelWithDebInfo / MinSizeRel | `/SUBSYSTEM:WINDOWS` + `/ENTRY:mainCRTStartup` | 不弹控制台，入口仍是 `main()` |

为什么不用 `WIN32_EXECUTABLE` 属性：**它不能按配置区分**，VS 这类多配置生成器上一设就是
所有配置都变 GUI 子系统。做法见 CMakeLists 里那段注释（`target_link_options` + 生成器表达式）。

想确认实际编出来的是哪个子系统：

```bat
dumpbin /headers deploy\bin\appQtPlayer.exe | findstr /i subsystem
:: Release 应该是 subsystem: Windows GUI；Debug 是 subsystem: Windows CUI
```

Release 下没有控制台，日志不会显示；要看日志就用 Debug 配置，或者重定向
（`appQtPlayer.exe > log.txt 2>&1`，stdout 句柄由命令行给，仍能落盘）。

### 3.4 部署（Windows）：为什么"直接拷 exe"打不开

**结论：不要裸用 windeployqt，用 `cmake --install`。** 一条命令：

```powershell
cd D:\...\CicadaPlayerNext\platform\QtPlayer
.\deploy_win.ps1                                  # = 配置+编译 Release + cmake --install + 依赖自检
```

> **不用先设 QTDIR**：脚本自己按 `$env:QTDIR`/`QT_DIR`/`QT_ROOT_DIR` → PATH 里的
> `windeployqt`/`qmake` → `<盘符>:\Qt\<版本>\<kit>`（官网安装包布局，选最新版本）的顺序找 Qt，
> 并把选中的路径打出来；选错了再显式设 `QTDIR` 覆盖。vcpkg 由 CMakeLists.txt 自己找
> （`$env:VCPKG_ROOT` → `where vcpkg` → 常见目录）。

> `deploy_win.bat` 是转发器（转调同一个 .ps1），两种都能用：
> `deploy_win.bat`（动态）/ `deploy_win.bat static [静态 Qt 路径]`（静态，见 **§3.8**）。
> **Windows 这两个脚本刻意只用 ASCII**（注释也是英文），别往里加中文：
> PowerShell 5.1 读**没有 BOM** 的 .ps1 时按 ANSI(GBK) 解码，中文注释会把后面的 ASCII 字节
> 一起吃掉，脚本就被改坏了 —— 典型症状是
> `无法将参数绑定到参数"Path"，因为该参数是空值`（`$MyInvocation.MyCommand.Path` 被啃成了 null）。
> .bat 同理保持 ASCII（cmd 控制台不是 UTF-8，中文 `echo`/`rem` 会变乱码）。
> 换行符：`.ps1` 用 LF（PowerShell 无所谓），`.bat` 目前也是 LF ——
> 仓库里实测过 Windows 11 / cmd 能跑 LF 的多行 bat（含 `goto` 标签），但 CRLF 才是批处理的原生
> 格式；万一遇到看不懂的语法错，用 §3.8 里那一行 PowerShell 把它转成 CRLF 再试。
> （原来的 .bat 只有一行，所以从来没有这个疑问。）
> 提示"禁止运行脚本"时用 `powershell -NoProfile -ExecutionPolicy Bypass -File .\deploy_win.ps1`。

或者手动两步：

```bat
cmake --build build\msvc --config Release --parallel
cmake --install build\msvc --config Release --prefix deploy
```

装出来的布局是 **Qt 的约定**（别改，改了 Qt 就找不到东西）：

```
deploy\bin\appQtPlayer.exe      ← 跑这个（exe + Qt6*.dll + libffmpeg.dll + pthread/SDL2 + qt.conf 都在 bin\）
deploy\qml\QtQuick\...          ← QML 模块（在插件和 qml 都在前缀根目录，exe 靠 qt.conf 找回来）
deploy\plugins\platforms\qwindows.dll
deploy\translations\
```

部署后运行会遇到的两类问题（都亲自踩过，原因不一样）：

| 现象 | 原因 | 修法 |
|---|---|---|
| 起来就退，日志里 `Module "QtPlayer" contains no type named "Main"` | **CMake 配置问题**：`qt_add_qml_module` 的资源前缀由 Qt 策略 `QTP0001` 决定，策略没设时前缀是 `/`，模块被嵌到 `:/QtPlayer/` —— 而 QML 引擎默认导入路径里只有 `:/qt/qml`，运行期找不到这个模块的 `qmldir`，只剩 C++ 注册出来的空模块。（Qt Creator 里能跑是因为它把**构建目录**放进了 `QML_IMPORT_PATH`，那里有生成好的 `QtPlayer/qmldir`；一拷走就没了。） | 本工程已在 CMakeLists 里加了 `qt_policy(SET QTP0001 NEW)`，模块改嵌到 `:/qt/qml/QtPlayer/`。**改完要删掉旧构建目录重新配置**：`rmdir /s /q build\msvc` |
| 起来就退，或提示缺 `QtQuick` / `QtQuick.Dialogs` | **部署不完整**：裸 `windeployqt` 只拷 Qt 的 DLL，**不拷 `qml/` 目录**（部署出来的目录里有 `platforms/`、`imageformats/`、`translations/`，就是没有 `qml/`） | 用 `cmake --install`（CMakeLists 里接了 `qt_generate_deploy_qml_app_script()`，按导入清单一起装 QML 模块），或 `windeployqt --qmldir <本目录> appQtPlayer.exe` |
| 提示缺 `libffmpeg.dll` / `pthread*.dll` / `SDL2.dll` / **`libxml2.dll`** | 这些是框架和 vcpkg 的运行时库，不是 Qt 的。注意 `libxml2.dll` 是 **exe 直接导入**的（框架用 libxml2 解析 XML），只拷 pthread/SDL2 就会漏掉它，另外它自己还要 `zlib1.dll`/`iconv-2.dll`/`charset-1.dll` | 现在 `cmake --install` 会一起装（见 CMakeLists 里的 `CICADA_VCPKG_RUNTIME_DLLS` 安装规则），`deploy_win.ps1` 的自检也会检查 `bin\libxml2.dll`；手工拷贝时别忘了这一串 |
| 提示缺 `VCRUNTIME140.dll` / `MSVCP140*.dll` / `VCRUNTIME140_1.dll` | MSVC 的 C++ 运行时（Qt 和 vcpkg 那些 DLL 都要它），**不是** Qt 也不是 vcpkg 的东西，所以上面的安装规则里没有它 | 目标机装「Microsoft Visual C++ 2015-2022 可再发行程序包 (x64)」，或者把 `<VS>\VC\Redist\MSVC\<版本>\x64\Microsoft.VC143.CRT\` 里的 `msvcp140*.dll`/`vcruntime140*.dll` 拷到 `bin\` |

程序启动时如果 QML 加载失败，会往控制台和日志里打一大段中文说明（见 `main.cpp` 的
`reportStartupFailure()`），里面直接写了当前这一行报错对应怎么修 —— 不会再出现"双击没反应、
什么提示都没有"。

调试期最省事的办法其实是**不部署**：直接用 Qt 的 bin 目录跑 `build\msvc\Release\appQtPlayer.exe`，
或者在 Qt Creator 里按 F5（它会自动把构建目录和 Qt 的 qml 目录放进导入路径）。

### 3.5 Linux / macOS 构建与部署

两个平台各有一个脚本（**首次使用要先给执行权限**，仓库里的文件默认没有 +x）：

```bash
cd <本目录>
chmod +x build_linux.sh build_macos.sh      # 或者用 `bash build_linux.sh` 调用

# Linux
./build_linux.sh                            # 配置 + 编译 Release（build/linux/appQtPlayer）
./build_linux.sh --deploy                   # 额外安装到 ./deploy（= 生成一棵独立目录树）
./build_linux.sh --clean --deploy /tmp/out  # 删构建目录重新配置，装到指定目录

# macOS
./build_macos.sh                            # 配置 + 编译 Release（build/macos/appQtPlayer.app）
./build_macos.sh --deploy                   # 额外安装到 ./deploy（内部会跑 macdeployqt）
```

两个脚本都：**只从环境变量/PATH 找 Qt**（`QTDIR` → `qmake6`/`qmake -query`，不写死路径）、
自动挑 Ninja 或 Make、编译完打印产物路径、`--deploy` 后做依赖自检。

**Linux 依赖**（Ubuntu/Debian 名，其它发行版类似）：

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
                 qt6-declarative-dev qt6-base-dev \
                 libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev \
                 libswscale-dev libswresample-dev libavfilter-dev \
                 libcurl4-openssl-dev libnghttp2-dev libxml2-dev
# 想要零拷贝（VAAPI → dmabuf → EGLImage）再加：
sudo apt install libva-dev libegl1-mesa-dev libdrm-dev
```

> **`libsdl2-dev` 在 Linux 上是必需的**：`framework/Linux.cmake` 里 `ENABLE_SDL` 被强制打开，
> 音频走的就是框架的 SDL2 渲染器（`SdlAFAudioRender2`）。缺了它配置阶段会直接 `FATAL_ERROR`
> 并把 `apt install` 命令打出来。
>
> **第三方库从哪来**：CMake 会找 `<repo>/external/install/<库>/Linux/x86_64/{include,lib}`
> （仓库自带的预编译）和系统默认路径。**本仓库当前没有 Linux 那套预编译**（只有 Android /
> Windows 的），所以 Linux 上实际用的是发行版包或 vcpkg（设了 `VCPKG_ROOT` 脚本会自动带工具链）。
> 用发行版包时有一个坑：框架里写的是 `#include <libxml/xmlreader.h>`，include 目录必须是
> "包含 `libxml` 子目录的那一层"（仓库预编译树是 `.../include/libxml2`，发行版包在
> `/usr/include/libxml2`，它不在默认搜索路径里），补一下即可：
> `EXTRA_CXX_FLAGS="-I/usr/include/libxml2" ./build_linux.sh`
>
> Linux 零拷贝还要求 **场景图跑在 OpenGL 上**（`QSG_RHI_BACKEND=opengl`），探测不到 libva/EGL
> 时 CMake 会把那条路编掉，自动走 CPU 路径（功能正常，只是费内存带宽）。

**macOS 依赖**：

```bash
xcode-select --install                        # clang + SDK
brew install cmake ninja ffmpeg curl nghttp2  # 框架要的三个第三方库
```

> `framework/macOSX.cmake` 找的是 `<repo>/external/install/<库>/Darwin/<arch>` 和
> **`/opt/homebrew`**（Apple Silicon 的 brew 前缀）。Intel Mac 的 brew 在 `/usr/local`，
> 那个路径没被探测 —— 脚本会把 `/usr/local` 一起放进 `CMAKE_PREFIX_PATH` 兜住；
> 实在不行就把库装到 `external/install/Darwin/x86_64/` 下。
> macOS 的零拷贝（VideoToolbox → `CVMetalTextureCache` → `MTLTexture`）用的是系统框架，
> 不需要额外依赖，场景图为 Metal 时自动生效。

**部署/运行**（两平台都走同一套 `cmake --install`）：

```bash
# Linux：Qt 是系统包时基本不用"部署"，直接跑就行；QTDIR 是官网 Qt 时建议 --deploy 生成独立目录
./build_linux.sh --deploy && ./deploy/appQtPlayer /path/to/video.mp4

# macOS：--deploy 出来的 .app 内部已带 Qt 框架和 QML 模块
./build_macos.sh --deploy && open ./deploy/appQtPlayer.app
# 想看框架日志（printf 到 stdout）就直接跑 bundle 里的可执行文件：
./deploy/appQtPlayer.app/Contents/MacOS/appQtPlayer /path/to/video.mp4
```

macOS 脚本在自检里会检查：bundle 里有没有 `Contents/MacOS/appQtPlayer`、`otool -L` 还引用不引用
外部 Qt/Homebrew 路径（引用了换机器就起不来）、以及 QML 模块在不在
（`Contents/Resources/qml/QtQuick/libqtquick2plugin.dylib`，有的 Qt 版本放在 `Contents/qml`）。
手工补救的办法：`macdeployqt <app> -qmldir=<本目录>`。

> 分发给别人还需要 Developer ID 签名 + 公证，脚本只保证**本机**可用。

> `VCPKG_TARGET_TRIPLET` 一定要给：vcpkg 只认 Visual Studio 生成器的平台名，
> 而 Qt Creator 用的是 **NMake Makefiles JOM**，它推不出目标架构 —— 会在配置阶段
> 打一句 "continuing without vcpkg" 然后把 vcpkg 关掉，接着 pthreads 就找不到了。
> （工程里在 NMake 这类生成器下会自动补一个 triplet，但命令行/kit 里显式给了就以你的为准。）

工具链换过之后建议先删掉 `build/` 再配置一次，避免旧缓存里的查找结果干扰。

> 这个工程和 `cmdline` 一样是**把框架当子目录一起编**的：`CMakeLists.txt` 里
> "框架部分"那一大段是从 `cmdline/CMakeLists.txt` 搬过来的（只把相对路径换成
> `${CICADA_ROOT}` —— 本工程比 cmdline 深一层），改框架编译选项时两处都要改。

---

### 3.6 无边框窗口（QWindowKit，用法照着 HuskarUI 来）

窗口没有系统标题栏，顶部那条是 `Main.qml` 里自己画的 `titleBar`：标题 +
最小化 / 最大化 / 关闭三颗按钮；全屏时整条隐藏（高度一起归零，因为播放器是贴着它
下边排的，见 `CicadaPlayerItem` 的 anchors）。

**没有自己写原生窗口代码**，直接复用 HuskarUI 用的那个 **QWindowKit**
（`3rdparty/qwindowkit`，就是 HuskarUI 仓库 3rdparty 里的同一份，Apache-2.0）。
HuskarUI 自己也是这么接的（它 `CMakeLists.txt` 里那几行 `set(QWINDOWKIT_BUILD_* ...)`），
所以拖动、贴边、双击最大化、右键系统菜单、多屏不同 DPI 的命中测试，行为跟 HuskarUI 的窗口一致。

| 在哪 | 做了什么 |
|---|---|
| `CMakeLists.txt` | `add_subdirectory(3rdparty/qwindowkit qwindowkit.out)` + `target_link_libraries(appQtPlayer PRIVATE QWindowKit::Quick)`。编成**静态库**：不产生额外 DLL，部署脚本和自检都不用改 |
| `main.cpp` | `QWK::registerTypes(&engine)`，必须在 `loadFromModule()` **之前**。QWindowKit 静态链接、没有 qmldir 也没有 QML 插件，QML 类型只能这样注册 |
| `Main.qml` | `WindowAgent { id: windowAgent }` + `visible: false`（先别显示），`Component.onCompleted` 里依次 `setup(root)` → `setTitleBar(titleBar)` → `setSystemButton(...)` → `setHitTestVisible(themeButton, true)` → `visible = true`。**不要自己写 `flags: Qt.FramelessWindowHint`**，理由见下 |

四个容易踩的点（`Main.qml` 里都有注释）：

0. **不要自己设 `Qt.FramelessWindowHint`**（HuskarUI 的 `HusWindow.qml` 和 QWindowKit README 的
   Qt Quick 示例都没设，去掉系统边框是 QWindowKit 按平台自己做的事）。自己设的后果实测过：
   Qt 会把窗口改成 `WS_POPUP` **并且去掉 `WS_THICKFRAME`**，而 Windows 上左边/右边/下边那三条
   原生 resize 区域正是 `WS_THICKFRAME` 提供的（各 8px，DWM 负责），QWindowKit 的 Windows 实现
   又走的是"只去掉顶部非客户区、保留另外三边原生边框"这条路（系统边框开着时它只清 `WS_SYSMENU`，
   不会替你补 `WS_THICKFRAME`）—— 两边一叠加就变成"**只有上边能缩放，左右下和四个角全都拖不动**"。
   实测：加 FramelessWindowHint 时 style=`0x96030000`（无 WS_THICKFRAME），WM_NCHITTEST 在
   左/右/下/角全部返回 `HTCLIENT`；去掉之后 style=`0x96C70000`（有 WS_THICKFRAME、
   WS_SYSMENU 已被 QWindowKit 清掉），左/右/下得到 `HTLEFT`/`HTRIGHT`/`HTBOTTOM`，
   四角得到 `HTTOPLEFT`/`HTTOPRIGHT`/`HTBOTTOMLEFT`/`HTBOTTOMRIGHT`，全部正常。
1. **顺序不能反**：先 `setup()`（这之后窗口才归它管），再 `setTitleBar()`，最后
   `setSystemButton()`。因为 `setTitleBar()` 换标题栏时会把之前注册的按钮全清掉
   （`abstractwindowcontext.cpp` 的 `removeSystemButtonsAndHitTestItems()`）。
2. **按钮点下去要干什么得自己写**。QWindowKit 只做命中测试和消息转发 —— Windows 上它把
   `WM_NCLBUTTONDOWN` 转成 `WM_LBUTTONDOWN` 发回 QML，**不会**替我们最小化 / 关闭窗口。
   HuskarUI 的 `HusCaptionBar` 也是三颗按钮各挂一个 `onClicked` 回调，同一个套路。
   注册系统按钮的收益是：悬停消息能到 QML（`WM_NCMOUSEMOVE` 会被转发），以及
   Windows 11 悬停最大化按钮时的"贴边布局"悬浮窗（命中测试返回 `HTZOOM`）。
   同理，标题栏里**非系统按钮**的可交互元素（我们那颗主题按钮）必须
   `setHitTestVisible(item, true)`，否则那片区域算拖动区、点击到不了 QML —— HuskarUI 里
   对应 `HusCaptionBar.addInteractionItem()`。
3. **macOS 上不画那三颗按钮**：QWindowKit 会把系统原生的"红黄绿"保留在左上角，
   再画一套既重复又难用。所以它们只在非 macOS 显示（`root.nativeCaptionButtons`），
   标题文字在 macOS 上往右让 78px。HuskarUI 的判断是
   `showMinimizeButton: Qt.platform.os !== 'osx'`，一个意思。

> **图形 API 一个字都没动**：QWindowKit 只管窗口系统这一层，场景图用哪个 RHI 后端、
> 用哪块 GPU 设备，仍然百分之百由 Qt 决定；组件里对 `QSGRendererInterface` 只有只读探针。
> 唯一需要注意的是 QWindowKit 在 Qt Quick 下要求 RHI 后端是 OpenGL / OpenGLES / D3D11 /
> D3D12（**Vulkan 下顶部边框会变成一条黑线**，这是它 README 里写明的已知问题），
> 而 Qt 在这些平台上的默认选择本来就落在这个范围里。
>
> QWindowKit 对 Qt 版本有要求：它 README 里推荐 **Qt 6.6.2 以上**（越新越好），因为它
> 依赖不少 Qt 内部实现，6.6.2 之前有些问题它绕不过去。本工程现在的验证环境是 Qt 6.11.1；
> 如果要降到 6.5 用，建议先把无边框这部分在目标 Qt 上试一遍。

> Windows 10 上如果看到窗口**顶部一条白线**：那是 D3D11/D3D12 走重定向表面导致的
> （QWindowKit README 的 Known Issues），可以启动前设环境变量
> `QT_QPA_DISABLE_REDIRECTION_SURFACE=1` 绕开；**OpenGL/OpenGLES/Vulkan 下千万别开**，
> 会直接渲染错乱。这事和 `QSG_RHI_BACKEND` 一样属于"渲染后端选择"，所以本工程**不替用户设**，
> 只在需要时由你自己加。
>
> 还有一条实测到的 Windows 行为（不是 bug，但看着会奇怪）：**实际窗口比 QML 里写的
> `width`/`height` 高出一个标题栏的高度**。实测 `Window { width: 1280; height: 720 }`
> 起来之后窗口矩形是 1294x758、客户区 1280x750、顶部非客户区 0 —— 也就是 QML 的坐标空间
> 变成了 1280x750，多出来的那 30px 就是 Qt 模型里那个"已经被 QWindowKit 去掉了的标题栏"。
> 界面本身是自适应的（控制条贴底、文字居中都在 750 这个真实空间里算的），所以不影响使用；
> 我们的 QWindowKit 就是 HuskarUI 那一份、CMake 选项也一样，所以这个行为跟 HuskarUI 一致。
> QWindowKit 自己有个 `setInternalWindowFrameMargins()` 的补偿函数，但当前版本里它被
> `return;` 短路掉了（注释写着"现在似乎不需要这个 workaround 了"），我们不做额外补偿。

---

### 3.7 主题（深浅色）：跟着 HuskarUI 那套思路搬过来

界面的**深浅两套颜色**放在单例 `QtPlayerTheme.qml` 里（一张扁平的调色板，每个键都是
`dark ? 深色 : 浅色` 的三元绑定），`Main.qml` 里**一个颜色字面量都没有** —— 全走
`QtPlayerTheme.xxx`，所以 `dark` 一变整屏自动重画，没有任何"重新应用主题"的代码。

对应关系（HuskarUI → 这里）：

| HuskarUI | 这里 | 说明 |
|---|---|---|
| `HusTheme`（单例 + `HusTheme.Primary.colorBgBase` 一堆 token） | `QtPlayerTheme` 单例 + `windowBg` / `videoBg` / `buttonBg` … | 那边是完整主题引擎（主题文件、几百个 token）；我们只有一屏播放器界面，扁平表够用 |
| `HusTheme.darkMode`（用户直接改的布尔） | `QtPlayerTheme.mode`（`"system"` / `"light"` / `"dark"`）+ 算出来的 `dark` | 多一层"跟随系统"，所以把"用户选的"和"最终生效的"分开 |
| `HusWindow.followThemeSwitch` + 系统深浅检测 | 默认 `mode: "system"` | 见下 |
| `HusWindow.setWindowMode(isDark)` | `Main.qml` 的 `applyWindowTheme()` → `windowAgent.setWindowAttribute("dark-mode", dark)` | **只通知窗口系统那一层**：QWindowKit 会写 `DWMWA_USE_IMMERSIVE_DARK_MODE`（Win10 1809+）并刷新系统菜单主题；macOS/Linux 各自由它实现 |
| `HusCaptionBar` 里那颗主题按钮（`themeCallback`） | 标题栏右边那颗太阳/月亮按钮 | 点一下深↔浅；它和设置页里那行「界面主题」是**同一个状态**，互相联动 |
| `HusCaptionBar.addInteractionItem()` | `Component.onCompleted` 里的 `windowAgent.setHitTestVisible(themeButton, true)` | **关键**：Windows 上整条标题栏都被当成拖动区（HTCAPTION），标题栏里**非系统按钮**的可交互元素必须这样声明一次，否则点它只会拖窗口 |

深浅是从哪儿来的：`Application.styleHints.colorScheme`（C++ 是 `QStyleHints::colorScheme`，
**Qt 6.5 起**有），它在三端都是真的去问系统 —— Windows 读注册表里的"应用模式"、macOS 读
`NSAppearance`、Linux 走桌面 portal / GTK。系统没表态（`Qt.Unknown`）时按浅色处理。

> **不要反过来去写 `colorScheme`**：它是"应用覆盖系统"的开关，写过之后读到的就是自己写进去的
> 值，"跟随系统"当场失效；而且写接口（`setColorScheme`）是 Qt **6.8** 才加的。所以这里只读。
> 也别在 `qml.exe`（离屏/offscreen）里验证这个值：offscreen 平台没有主题探测，一定返回
> `Qt.Unknown`；要用真实的 windows 平台插件跑（窗口可以 `visible: false`，不会弹出来）。

几个说清楚的行为：

* **深浅色只作用于"没选片子"的那块 UI**：窗口底色、提示文字、控制条、设置页 —— 都在
  `windowBg` 上，切主题看得最明显的就是这里。
* **一旦选了片子，播放区整块是黑的**（`QtPlayerTheme.videoBg = #000000`，见 `Main.qml` 里
  `root.color` 那条按"有没有片源"切换的绑定）：画面比窗口小时露出来的也是黑边，
  浅色主题下不会出现"视频后面一片白"。同时**只有顶部标题栏还是 `windowBg`** ——
  也就是"播放之后就只剩顶部有深浅色"。
* 标题栏底色**和窗口底色同色**（都用 `windowBg`），不单独配一个：
  没选片子时顶栏和中间连成一片（不会"上面一条、下面一块"）；选了片子后窗口变黑、顶栏不变。
  所以标题栏和播放区之间也不需要分隔线。
* 左上角那几行诊断文字会跟着底色换色（`diagText` ↔ `onVideoText`），
  否则浅色主题那套深色字压在黑画面上会看不清。
* 视频画面**不铺到标题栏底下**（`CicadaPlayerItem` 的 anchors 是 `titleBar.bottom`，
  不是 `fill: parent`）—— "沉浸"指的是标题栏自己的底色，不是让画面钻到顶上去。
* **默认跟随系统**（和 HuskarUI 的 `followThemeSwitch: true` 一致）。所以在一台"浅色模式"的
  Windows 上，第一次跑起来界面就是浅色的 —— 想固定深色：设置页 →「界面主题」→「深色」，
  或者点标题栏那颗月亮/太阳。
* 设置页里的三选一：**跟随系统 / 浅色 / 深色**；标题栏那颗按钮则是在深↔浅之间**直接切**
  （从"跟随系统"按一下会落到**显式**选定的相反色 —— 用户按按钮的意思就是"我就要这个颜色"）。
* 和别的设置一样**只在本次运行内有效**，没有落盘（要持久化就 `QSettings` 加几行）。
* **系统原生对话框（选文件那个）跟着操作系统走，不跟我们的主题** —— Windows 的
  `IFileDialog` 自己读系统设置。强制深色时它可能还是浅色，这正常，别去改它。
* 组件本身（`CicadaPlayerItem`）**不认识主题**：它只画视频画面，界面配色是应用层的事。

### 3.8 Windows：一条命令构建（静态 / 动态 Qt 可切）

配置 + 编译 + 安装 + 依赖自检都在脚本里，**两种 Qt 链接方式用参数切换**。

#### 怎么用

```bat
cd CicadaPlayerNext\platform\QtPlayer

deploy_win.bat                     :: 动态 Qt -> build\msvc        -> deploy\
deploy_win.bat static              :: 静态 Qt -> build\msvc-static -> deploy-static\
deploy_win.bat static D:\Qt-Static\msvc       :: 静态 Qt，并**显式给出**静态库装在哪
deploy_win.bat D:\Qt-Static\msvc   :: 同上（省略 static，模式跟着前缀走）
deploy_win.bat -Static             :: 同上（static / -Static 两种写法都认）
deploy_win.bat -QtPrefix D:\Qt-Static\msvc    :: 显式指定 Qt 前缀（模式跟着前缀走）
deploy_win.bat -DeployDir out      :: 换输出目录
deploy_win.bat -h                  :: 用法

:: 静态 Qt 的位置**默认从环境变量找**（推荐：设一次，以后只要 deploy_win.bat static）
set QT_STATIC_DIR=D:\Qt-Static\msvc        :: cmd，当前窗口（setx 写进用户环境变量）
$env:QT_STATIC_DIR = "D:\Qt-Static\msvc"   :: PowerShell

:: 也等价于直接调 PowerShell（参数完全一样）
.\deploy_win.ps1 -Static
.\deploy_win.ps1 -Static -QtPrefix D:\Qt-Static\msvc
```

* **`deploy_win.bat` 是入口，`deploy_win.ps1` 是脑子**：bat 只做"把参数翻成人话再转发"
  （`static [前缀]` / `shared` 映射成 `-Static` / 去掉第一个词；其余参数原样转发），
  真正的逻辑（找 Qt、版本排序、模式判定、目录选择、自检）全在 ps1 里 ——
  **bat 里没有一行构建逻辑**，不会出现两套逻辑各自漂移。
* bat 是带 `goto` 标签的多行 cmd（约 2.8 KB），换行符是 **LF**。本仓库实测过
  Windows 11 / cmd 能正常跑 LF 的多行 bat（含标签），所以留 LF（和仓库其它文件一致）。
  万一你的 shell 报奇怪的语法错，转成 CRLF 即可：
  `powershell -NoProfile -Command "(Get-Content -Raw .\deploy_win.bat) -replace '\r?\n','\r\n' | Set-Content -NoNewline -Encoding ascii .\deploy_win.bat"`
* 参数含空格或引号（例如部署目录带空格）时，直接调 ps1。

#### 静态 / 动态怎么决定

**跟着 Qt 前缀走，不用你手动传 CMake 开关**：

* 脚本按"**`<前缀>\bin` 下有没有 `Qt6Core.dll`**"判断这个 Qt 是静态还是动态版，
  然后自己给 CMake 传 `-DCICADA_QT_STATIC=ON/OFF`。
* **动态 Qt** 的查找顺序：`-QtPrefix` → `QTDIR`/`QT_DIR`/`QT_ROOT_DIR` → PATH 上的
  `windeployqt`/`qmake` → `<盘>:\Qt\<版本>\<kit>`（同版本取最新）。
* **静态 Qt 单独一条路**（见下一小节）：`-QtPrefix` → **`QT_STATIC_DIR`**（别名
  `CICADA_QT_STATIC_DIR`）→ `<盘>:\Qt-Static\msvc`。
  **它不会退回动态 Qt**：`-Static` 时找不到静态库就**直接报错退出**，
  绝不会"悄悄编出一个动态版放进 `build\msvc-static`"。
* `-QtPrefix` 给的是动态 Qt、同时又写了 `-Static` 这种自相矛盾的组合：
  **前缀优先**（照动态编译），但会打一条黄色 `[WARN]` 说明这件事。
* **两个模式用不同的构建目录**（`build\msvc` vs `build\msvc-static`），这是刻意的：
  `/MD` 和 `/MT`、插件集都不同，共用一个 CMake 缓存会编出四不像。

#### 静态 Qt 装在哪：环境变量（默认）或参数

静态库的位置**不用改脚本**，两种给法：

| 给法 | 命令 | 说明 |
|---|---|---|
| 环境变量（默认） | `set QT_STATIC_DIR=D:\Qt-Static\msvc` 然后 `deploy_win.bat static` | 推荐。`set` 只影响当前窗口，`setx` 写进用户环境变量（新开的窗口才生效） |
| 命令行参数 | `deploy_win.bat static D:\Qt-Static\msvc` | 只影响这一次；bat 把前缀**通过环境变量传给子进程**（`setlocal` 里设，不会污染你自己的环境变量） |
| ps1 参数 | `.\deploy_win.ps1 -Static -QtPrefix D:\Qt-Static\msvc` | 直接调 ps1 时用 |

* 别名：`CICADA_QT_STATIC_DIR` 与 `QT_STATIC_DIR` 等价（前者是早期名字，保留兼容）。
* **变量设了但路径不对**（给错目录、或者那个 Qt 其实是动态版）→ 脚本**报错退出**并打印
  那个变量的值，不会"忽略它、换一个 Qt 编"。判据：
  `<前缀>\bin\windeployqt.exe` + `<前缀>\lib\cmake\Qt6\Qt6Config.cmake` 必须都在。
* 脚本会把自己**实际用的是哪个 Qt、从哪儿找到的**打在第一行：
  `== Qt: D:\Qt-Static\msvc  [static]  (static search)`。

#### 静态构建需要什么

* 一个**静态 Qt**（`configure -static -static-runtime`）。本机是 `D:\Qt-Static\msvc`（Qt 6.11.2）。
  `-static-runtime` 意味着它用 **/MT**，所以主程序和框架那一堆静态库也必须走 `/MT`
  —— 这件事 `CMakeLists.txt` 在**任何 target 创建之前**就设好了（`CMAKE_MSVC_RUNTIME_LIBRARY`），
  设晚了只对可执行文件生效，会报 `LNK2038` + `LNK2005`。
* **vcpkg 不需要换成 `-static` triplet**：我们链的是导入库，导入库里没有 CRT 的
  `/DEFAULTLIB`，所以 `/MT` 的程序能正常链 `/MD` 编的导入库，运行时照旧拷那些 DLL。
  真要做到"单文件 exe"才需要 `x64-windows-static`，那是另一件事。
* 静态 Qt 下 QML 插件和平台/图片格式插件是**显式链接**进去的
  （`qt_import_qml_plugins` + `qt_import_plugins`），**素材里的 SVG 靠的就是这条**
  —— 少了它所有 `.svg` 图标都画不出来，而且是**运行期**错、不是编译错。

#### 两种模式实测对比（Qt 6.11.2 / MSVC）

| | 动态 | 静态 |
|---|---|---|
| `appQtPlayer.exe` | 4.2 MB（4,431,872 字节） | **44.8 MB**（46,975,488 字节） |
| exe 旁的 `Qt6*.dll` | 一堆 | **一个都没有** |
| `PATH` 里没有 Qt 时运行 | `0xC0000135` 起不来 | **正常启动** |
| 部署目录 | `deploy\` | `deploy-static\` |

静态版旁边仍然需要 `libffmpeg.dll`、`Danmaku.dll`、`SDL2.dll`、`pthreadVC3.dll`、
`libxml2.dll` 等 —— **它们不是 Qt**，静态 Qt 不会替你把它们包进去。
另外会自动多出 `jpeg62/libpng16/libwebp/tiff/zstd` 这些：静态链接把 Qt 的
SVG/图片格式插件拖进来后，MSBuild 会按导入库的 `IMPORTED_LOCATION` 自动拷到 exe 旁边。

#### 脚本干了哪四步

1. **configure**（每次都做）：已有的 `CMakeCache.txt` 不能证明这个目录可用（配置失败
   一半会留下缓存但没有工程文件，下一步就报 `MSB1009: project file does not exist`）；
2. **build** `Release`；
3. **install** 到部署目录 —— 动态 Qt 下 Qt 的运行库、`qml/` 模块、`platforms/` 插件
   都由 `CMakeLists.txt` 的 install 规则装好（**裸 `windeployqt` 不会拷 `qml/`**，
   这正是"部署出去的程序一启动就退出"的常见原因）；静态 Qt 下这一步只拷框架自己的 DLL；
4. **依赖自检**：逐个检查 exe 旁边该有的文件，缺了就用红字说明常见原因并返回非 0。

### 3.9 静态 / 动态之外：同一个 exe 能不能再包一层 DLL

可以做（把 Qt + 主程序打进 `XxxCore.dll`，入口 exe 只调一个导出的
`extern "C" int cicadaQtMain(int, char **)`），但**要看目的**：

| 想要 | 合适吗 |
|---|---|
| 文件更少 / 更小 | ❌ 反而是 1 个文件变 2 个，总量还更大 |
| exe 极小、主体可单独复用或热替换 | ✅ 合适 |
| 给别的程序 / 别的语言调用 | ✅ 合适（跨边界只走 C ABI） |
| 缓解 LGPL（静态链接 Qt 要允许用户重新链接） | ✅ 比单体静态 exe 好，换 DLL 即可重链接 |

三条硬约束：exe **绝对不能链接 Qt**（否则静态 Qt 的全局状态会有两份）；
跨边界只传 C 类型；两边 CRT 一致（都是 `/MT`）。本项目**目前没做**这一层。

---

## 4. 为了零拷贝对框架做的改动（全部向后兼容）

| 位置 | 改动 | 为什么 |
|---|---|---|
| `framework/render/renderFactory.h/.cpp` | 新增 `videoRenderFactory::setRenderCreator()`：平台可以注册自己的视频渲染器 | 框架自带的渲染器要么自建 SDL 窗口，要么把帧丢掉，Qt 应用都拿不到帧 |
| `framework/utils/hwDeviceBridge.h/.cpp`（新） | 平台可以把**已有的 GPU 设备**交给 FFmpeg | 零拷贝要求解码纹理和 Qt 在同一个设备上（D3D11） |
| `framework/codec/avcodecDecoder.cpp` | 优先使用外部设备；直通输出日志通用化（不再只在 Windows） | 同上；macOS/Linux 的直通输出也走同一套逻辑 |
| `framework/base/media/PBAFFrame.h` | 实现 `clone()`（原来是 `return nullptr`） | Qt 组件的帧交接必须留一份副本；实现是 `CVPixelBufferRetain`，零拷贝 |
| `framework/codec/avcodecDecoder.h` | Apple 也定义 `CICADA_HW_*`（VideoToolbox），**但默认不激活** | 给"想用 FFmpeg videotoolbox 直出 CVPixelBuffer"的应用留一条路 |
| `framework/codec/decoderFactory.cpp` | Apple 上打开 `video.decoder.ffmpeg_videotoolbox` 时优先用 `avcodecDecoder` | 同上；默认行为不变，其它 Apple 应用不受影响 |
| `framework/utils/property.h` | 新增 `video.decoder.ffmpeg_videotoolbox` | 上面那个开关 |
| `framework/utils/AFMediaType.h` + `framework/utils/ffmpeg_utils.c` | 新增 `AF_PIX_FMT_VAAPI`（=902）并补进 `pix_fmt_pair_table` | Linux 硬解直通的帧是 `AV_PIX_FMT_VAAPI`（`data[3]` 是 `VASurfaceID`）。**不登记的话 `AVPixFmt2Cicada()` 会把它变成 `AF_PIX_FMT_NONE`**，呈现方既认不出这是 GPU 帧、也没法拒绝它 —— 把 surface ID 当像素指针用就是崩溃或满屏花屏。登记之后 `CicadaVideoTexture` 的 CPU 回退能明确拒绝这类帧 |

---

## 5. 当前进度与后续

* [x] 框架侧三个钩子
* [x] Qt 组件骨架：QML 元素、属性/信号/方法、跨线程帧交接、等比适配、诊断信息
* [x] **Windows 零拷贝**（D3D11 视频处理器）；Qt 选的设备带视频接口时走零拷贝，不带时
      自动走硬解 copy-back（见 §2.2，设备选择始终归 Qt）
* [x] **macOS 零拷贝**（框架 VTB 解码器直出 BGRA CVPixelBuffer → CVMetalTextureCache → MTLTexture）
* [x] **Linux 零拷贝**（VAAPI VPP 转 BGRA → dmabuf → EGLImage → GL 纹理）；需要场景图为
      OpenGL 后端 + libva/EGL 头（CMake 自动探测，缺依赖时自动编出 CPU 路径）
* [x] **HDR 色调映射**（Windows）：把帧的 `color_trc`/`color_primaries` 翻译成
      `DXGI_COLOR_SPACE_TYPE` 交给视频处理器（输入 PQ/HLG/P2020，输出 SDR BT.709），
      HDR 片源在 SDR 输出下由驱动做色调映射，不再发灰
* [x] **自动降级安全**：只有在纹理后端确认零拷贝可用时才打开
      `video.render.hw.direct_texture`；否则走 copy-back。运行中零拷贝失败也不会把
      "未下载的 GPU 帧"喂给 swscale（那是野指针），而是保持上一帧并打日志
* [x] CPU 回退路径（三端都能播）
* [x] **三端构建脚本**：`deploy_win.ps1`（Windows，顺带部署）、`build_linux.sh`、
      `build_macos.sh` —— 都只从环境变量/PATH 找 Qt（不写死路径），并带依赖自检
* [x] **部署能独立运行**：`qt_policy(SET QTP0001 NEW)`（否则 QML 模块嵌在 `:/QtPlayer/`，
  默认导入路径里没有它 —— 部署出去就是 `Module "QtPlayer" contains no type named "Main"`）
  + `qt_generate_deploy_qml_app_script()`（否则不拷 `qml/` 目录，见 §3.4）
* [x] **seek 不卡死**（三端通用，属于框架层的既有问题）：
  * 框架的**精确 seek** 很贵（清包队列 → 重新找关键帧 → 解码到目标帧 → 等目标帧渲染出来
    才算结束）。像拖动进度条那样每秒发几十次 seek，会把消息队列塞满 `MSG_SEEKTO`、
    读写/解码线程反复 flush，表现就是拖动时卡住，偶尔还把 demux 线程停在半路不回来。
    → 组件侧 `CicadaPlayerItem::seek()` 改成 **单飞 + 最新优先**：同一时刻只让一个 seek 在跑
    （用框架的 `Seeking`/`SeekEnd` 回调判定），拖动过程中只记住**最后**一个位置，上一个
    结束再发。调用方可以随便高频调用。
  * 再加一道 **seek 看门狗**（2 秒）：框架迟迟不发 `SeekEnd` 就清掉组件自己的"正在 seek"
    状态并打一条 `W` 日志 —— 至少界面不会因此彻底失去响应（否则一次卡住的 seek 会让
    组件此后再也不发 seek）。这条日志本身就是"seek 卡在框架里"的证据。
  * 框架侧修了一处：`SMPMessageControllerListener::ProcessSeekToMsg()` 里
    `PLAYER_STOPPED` 被前面那个"prepare 之前保留 mSeekPos"的分支拦下了，导致"停止状态下
    seek"既不执行、也不清状态 —— `isSeeking()` 永远为真、`mSeekNeedCatch` 一直留着，
    之后重新播放时所有早于该位置的包都被丢掉（画面出不来的卡死）。现在 STOPPED 会走到
    下面那个本来就为此写的 `ResetSeekStatus()` 分支。
* [x] **硬解开关**（`player.hardwareDecoding`）：关掉即软件解码 + CPU 回退。改这个开关会
      **先把"零拷贝还是 CPU 路径"在渲染线程上重算完，再重开片源并 seek 回原位置** ——
      顺序反了会出现"硬解打开了、零拷贝却一直关着"（因为打开直通开关要读重算后的结论）
* [x] **演示界面的设置页 + 控制条自动隐藏**（见开头那张表）
* [x] **无边框窗口**：复用 HuskarUI 那套 QWindowKit（静态链接，见 §3.6）—— 自绘标题栏 +
  自绘最小化/最大化/关闭，拖动、贴边、双击最大化、右键系统菜单、**四边和四个角的拖拽缩放**
   全交给它；macOS 上保留系统原生的"红黄绿"。图形 API 完全没有介入
   （踩过的坑：自己加 `Qt.FramelessWindowHint` 会让左右下三边和四角都不能缩放，
   原因和实测数据见 §3.6 第 0 条）
* [x] **主题深浅色**（见 §3.7）：调色板在 `QtPlayerTheme` 单例里，默认**跟随系统**
  （`Application.styleHints.colorScheme`，Qt 6.5+），设置页里可强制浅色/深色，标题栏那颗
  太阳/月亮按钮直接切；切换时用 QWindowKit 的 `"dark-mode"` 把窗口边框/系统菜单一起带过去。
  `Main.qml` 里已经没有颜色字面量
* [x] **换片子不会再"点「打开」没反应"**：文件对话框改成**每次现建一个、关掉就销毁**
  （`Component.createObject`），不再复用同一个 `FileDialog` 对象 —— 复用时 Qt 内部那套
  "想显示 / 真的显示了"两段式状态和原生对话框句柄会交错，第二次或第四次点击就可能
  静默失效（详见 `Main.qml` 里 `chooseFile()` 的注释）。另外命令行的 `Ctrl+O`、
  拖拽、对话框三条入口仍然都走同一个 `playFile()`
* [x] **旋转 / 镜像**：视频自带的旋转元数据（手机竖屏视频）自动生效，QML 侧还有
      `extraRotation`（0/90/180/270 叠加）和 `mirror` 两个属性；用 `QSGTransformNode` +
      纹理坐标变换实现，仍然是 Qt 场景图在做变换
      （不叫 `rotation`：`QQuickItem` 自己就有 `rotation`，同名会盖掉基类属性）
* [x] **Qt 6 兼容性收尾**（本轮补齐，都是会直接编不过的硬问题）：
  * Qt 6 删掉了 `createTextureFromNativeObject()` → 新增 `CicadaQtTextureWrap`
    （6.8+ 的 `fromNative()` / 6.5~6.7 的 QRhi 兜底）；
  * Qt 6 强制 **C++17** → 只给 `appQtPlayer` 目标设 `CXX_STANDARD 17`，框架仍是 C++11；
  * `playerListener` 在**全局命名空间**（头文件里曾前向声明成 `Cicada::playerListener`，
    会编不过）；`MediaPlayer` 上**没有**读取播放状态的接口 → 状态改用 `StatusChanged`
    回调同步；
  * 直通开关必须写 `globalSettings::getSetting().setProperty()`，写成 `QObject::setProperty()`
    只会挂个动态属性、解码器根本看不到（表现是"日志说零拷贝可用，实际一直 copy-back"）；
  * Linux 硬解帧的像素格式要在 `pix_fmt_pair_table` 里登记（新增 `AF_PIX_FMT_VAAPI`），
    否则 `AVPixFmt2Cicada()` 会把它变成 `AF_PIX_FMT_NONE`。
* [ ] HDR 直通（真 HDR 输出）：需要 10bit + PQ 的输出纹理以及 Qt 6.8+ 的色彩管理；
      当前是"色调映射到 SDR"，画面正确但不是 HDR
* [ ] 纹理缓存：现在每帧都新建一个 `QSGTexture` 包装同一个原生纹理（包装很轻，但可以缓存成
      一个，只在输出纹理或尺寸变化时重建）。改的时候注意 `QSGTexture` 的删除必须发生在渲染线程。
* [ ] Linux 的 `GL_TEXTURE_EXTERNAL_OES` 变体：直接导 NV12 的 dmabuf + 自定义 `QSGMaterial`
      （少一次 VPP 转换，但需要自己写采样着色器），作为可选项保留。

### 第一次跑起来看什么

1. 用左下角「打开」按钮选一个本地视频（或者把文件拖进窗口、启动时带路径）；
2. 界面左上角显示 `零拷贝: 是/否` + 后端名 / `文件:` / `解码设备:` / `视频:` / `状态:`；
3. 控制台里按顺序会出现：

```
Qt scene graph graphics API: Direct3D11 (RHI)
D3D11VA: hardware decoding enabled for hevc (hw pixel format d3d11)
captured Qt's D3D11 device for zero-copy decoding: Qt D3D11 device (NVIDIA GeForce ...)
D3D11 zero-copy is ready: decoded NV12/P010 textures are converted to RGBA ... 
first zero-copy frame: 3840x2160 NV12 texture -> RGBA on the GPU -> Qt
```

任何一条以 `W`/`E` 开头且带 "falling back to the CPU path" 的日志都说明画面走的是
"下载 + 上传"（功能正常，只是费内存带宽），具体原因会写在同一行。

## 6. 已知限制

* 一个进程里只有一套 Qt 场景图设备，所以**多窗口同时播不同硬解视频**时会共用同一个
  解码设备（框架侧本来就是单设备模型）。
* 暂停时窗口改变尺寸靠"重绘上一帧"处理；如果解码器已经释放了最后一帧，会短暂空白。
* **Windows 上零拷贝能不能成立取决于 Qt 选的 D3D11 设备是否带视频接口**（`ID3D11VideoDevice`）。
  程序**不会**去换 Qt 的设备：带不上时它自动保留硬解（FFmpeg 自建设备 + copy-back）并打日志，
  画面完全正常，只是多一趟 GPU→内存→纹理的搬运。日志里会写明原因，UI 上的"零拷贝: 否"
  和"后端: CPU copy-back"就是对得上的。
* Windows 上 `CICADA_QT_D3D11_OUTPUT_FORMAT` **必须是** `R8G8B8A8_UNORM`，不能改成
  `B8G8R8A8_UNORM`（即使画面颜色看起来不对也不要改这个）：`QSGTexture` 没有像素格式
  参数，Qt 只会按 RGBA8 建 SRV，而有类型的 D3D11 纹理格式对不上就是 `E_INVALIDARG`
  （日志里 `Failed to create srv: COM error 0x80070057`），表现是**解码正常但画面全黑**。
  颜色通道由视频处理器按声明的格式写，不会错位。Linux 上对应的开关是
  `src/CicadaTextureVAAPI.cpp` 里 `VA_FOURCC_BGRA`（改成 `VA_FOURCC_ARGB`）。
* 别的线程**不能**直接调 `QQuickItem::update()`：Qt 只允许 GUI 线程或
  `updatePaintNode()` 里调度重绘，其它线程调用会被拒绝并打印
  `Updates can only be scheduled from GUI thread or from QQuickItem::updatePaintNode()`，
  结果是画面一帧都不更新。框架 VSync 线程那条路走的是队列投递（见 `CicadaPlayerItem.cpp`）。
* QML 里的小工具函数（`formatTime`/`seekAt`/`playFile`/`sourceName`）都声明在**根对象**上：
  QML 的静态作用域只把根对象的函数带进整个组件，声明在非根对象（比如控制条那个
  `Rectangle`）里的函数，子对象的绑定找不到，会报 `ReferenceError: xxx is not defined` 并刷屏。
* Qt 6.5~6.7 的兜底路线（`QRhiTexture::createFrom`，见 §2.1）只在 GL 后端用到
  `native.layout = GL_TEXTURE_2D`（`CicadaQtTextureWrap.cpp` 里那处硬编码常量，
  等于 `0x0DE1`）。6.8 及以上走 `fromNative()`，和这个参数无关。

## 7. 用到的 Qt API 与版本（编译报错时先看这里）

| API | 引入版本 | 用在哪 |
|---|---|---|
| `QQuickItem::updatePaintNode()` / `QSGSimpleTextureNode` / `QSGTransformNode` | Qt 5 起就有 | 组件的绘制与旋转 |
| `Application.styleHints.colorScheme`（C++ 是 `QStyleHints::colorScheme`） | **Qt 6.5** | 读系统深浅色（主题跟随系统，见 §3.7）。**只读**；写接口 `setColorScheme` 是 Qt 6.8 才有，本工程不用 |
| `QNativeInterface::QSG{D3D11,Metal,OpenGL}Texture::fromNative()` | **Qt 6.8** | 把原生纹理包成 `QSGTexture`（首选路线，见 §2.1） |
| `QRhiTexture::createFrom(NativeTexture)` + `QQuickWindow::createTextureFromRhiTexture()` | Qt 6.0 | 上面那条的兜底（Qt 6.5~6.7；需要 `Qt6::GuiPrivate` 才能拿到 `rhi/qrhi.h`，CMake 里按需自动链） |
| `QSGRendererInterface::DeviceResource` / `DeviceContextResource` / `graphicsApi()` / `RhiResource` | Qt 6.0 | **只读地**取 Qt 当前用的 D3D11/Metal 设备与 RHI，判断后端；不指定、不替换任何设备 |
| `QQuickWindow::sceneGraphInitialized` / `sceneGraphInvalidated` | Qt 5 起就有 | 在渲染线程上做准备/收尾 |
| `QOpenGLContext::getProcAddress` / `QOpenGLFunctions` | Qt 5 起就有 | Linux 上取 EGL/GL 扩展函数（不直接链 libGL） |
| `QQuickWindow::graphicsApi()` | Qt 6.0 | 启动日志里打印当前图形后端 |
| `QQuickWindow::createTextureFromImage()` | Qt 5 起就有 | CPU 回退路径上传 `QImage` |

* 组件自己的源码（`src/`、`main.cpp`）没有用到任何 Qt 私有头。**但静态链进来的 QWindowKit
  用了**（`QtQuick/private/qquickitem_p.h` 之类），所以配置阶段必须有 Qt 的 Private 模块：
  `Qt6CorePrivate` / `Qt6GuiPrivate` / `Qt6QuickPrivate`。官网安装的 Qt 都带，
  缺了的话 QWindowKit 会在配置/编译阶段报错（`find_package(Qt6CorePrivate)` 失败）。
* 无边框窗口那部分是 **QWindowKit** 提供的（不是 Qt 也不是框架的东西）：
  QML 里是 `import QWindowKit` 的 `WindowAgent`，C++ 里是 `QWK::registerTypes()`，
  细节见 §3.6。
* **C++ 标准必须是 17**：Qt 6 在 `QtCore/qcompilerdetection.h` 里直接
  `#error "Qt requires a C++17 compiler"`。框架（mediaPlayer/demuxer/codec/...）仍然按 C++11 编，
  所以 `CMakeLists.txt` 里全局保持 `CMAKE_CXX_STANDARD 11`，只在 `appQtPlayer` 这个目标上用
  `CXX_STANDARD 17` 覆盖（MSVC 还需要 `/Zc:__cplusplus`，由 `Qt6::Quick` 传递的
  `Qt6::Platform` 自动带上）。
* `createTextureFromNativeObject()` 是 Qt 5 的接口，Qt 6 已经删除 —— 这是本轮最大的改动之一，
  历史版本代码里看到它请直接换成本工程的 `CicadaQtTextureWrap`。
