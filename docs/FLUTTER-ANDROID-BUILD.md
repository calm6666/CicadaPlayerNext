# 编译 Android 的 Flutter 测试 APK —— 步骤与踩过的坑

> 目标：改完内核之后，能在**真机**上装一个 Flutter APK 去测零拷贝播放。
> 本文里的每条命令都在本机（Windows，JDK 21，Android SDK `D:\Android\Sdk`，
> NDK 25.2.9519653，Flutter `D:\flutter`）实测过。
>
> 【为什么需要这份文档】Flutter 这条路有 **4 个前置坑**，其中 3 个与本仓库代码无关
> （是环境/版本的），不看这份文档会一个下午都卡在"编不过"上。

---

## 0. 一句话流程

```
① 改内核 ──► ② 编 aar（约 2.5 分钟）──► ③ 拷 aar 到 Flutter 插件的 libs/ ──► ④ flutter build apk
                                                                              └► ⑤ adb install / flutter install
```

**关键点**：Flutter 插件不直接编内核，它依赖一个 **`.aar`**。所以**每次改动内核 C++/Java**
都必须重跑 ②③，否则 APK 里装的是旧内核（这是最容易犯的错：改了代码、APK 却没变）。

---

## 0.1 三层代码住在哪一层（"到 Flutter 了为什么还要写 Java"）

一句话：**Flutter 只是 UI 与业务那一层，播放器这种"要调平台能力"的东西在 Flutter 里天生是
"一半 Dart + 一半平台原生"，再加上本来就有的 C++ 内核，一共三层。**

```
platform/Flutter/
├─ lib/                                  ★ 第一层：Flutter(Dart) —— 这才是"Flutter 代码"
│    flutter_cicadaplayer.dart   578 行    FlutterCicadaPlayer / CicadaTextureView / 全部回调 typedef
│    flutter_avpdef.dart         248 行    AVPMediaInfo / AVPTrackInfo / EventChanneldef 等数据类
│    flutter_cicadaplayer_factory.dart 12 行 工厂
│
├─ android/src/main/java/…               ★ 第二层：Android 原生半边（Java，必须有）
│    FlutterCicadaPlayer.java    899 行    MethodChannel / EventChannel 的 Android 实现
│    FlutterCicadaPlayerPlugin.java 101 行 插件注册 + 建销毁纹理（工厂通道）
│    PlayerSurface.java          100 行    SurfaceProducer（零拷贝纹理）
│    FlutterCicadaPlayerView.java  69 行   旧平台视图（保留）
├─ ios/Classes/…                         ★ 第二层：iOS 原生半边（ObjC，同职责）
│    CicadaPlayerFactory.m       898 行 / FlutterCicadaPlayerTexture.m 148 行 / …
│
└─ example/lib/                          ★ 第一层：写 App 时的 Flutter 代码
     main.dart                   392 行    本轮新写的测试 App（只 import flutter + 插件）
     （page/ model/ widget/ … 是旧 demo，尚未做 Dart 3 迁移，见 §4.1）

framework/ + platform/Android/source/…/java/com/cicada/player/…   ★ 第三层：内核（C++ / Java SDK）
                                                                  两个平台共用，和 Flutter 无关
```

**为什么第二层必须是 Java/ObjC，不能用 Dart 替代**：

1. Dart **不能直接调 C++/JNI**；`libCicadaPlayer.so` 是 JNI 库，Android 上必须由 Java 加载并持有。
2. 纹理/画面是平台对象：Android 的 `TextureRegistry.SurfaceProducer` + `Surface`、
   iOS 的 `FlutterTexture` + `CVPixelBuffer`，Dart 里没有对应概念。
3. 事件回传要走 `MethodChannel`/`EventChannel`，这一端必须有平台实现来 `success(...)`。

所以 **Dart 半边的职责是"声明 API + 收发消息"，Java/ObjC 半边的职责是"真的干活"**。
调用链（Dart 里点一下"播放"发生了什么）：

```
Dart   CicadaTextureView(onCreated:) → FlutterCicadaPlayer.prepare()
         ↓  MethodChannel('flutter_cicadaplayer').invokeMethod('prepare')
Java   FlutterCicadaPlayer.onMethodCall("prepare") → mCicadaPlayer.prepare()
         ↓  JNI
C++    内核解码/渲染，把画面写进 PlayerSurface 给的那张 Surface（GPU→GPU，零拷贝）
         ↑  EventChannel('flutter_cicadaplayer_event')
Java   mEventSink.success({method:"onRenderingStart", …})
Dart   _onEvent → onRenderingStart 回调 → setState → 画面出现
```

**那我（本轮）改的 Java 是什么**：不是"给 Flutter 写业务"，而是**修这个插件自己的平台半边**——
它里面还留着 Flutter 3.29 已经删掉的 Android **v1 embedding**（`PluginRegistry.Registrar`）、
`PlayerSurface` 的纹理回调从来没注册过、SDK 新加的 `onSubtitleHeader` 没实现。
这些不修，Dart 那半边写得再对也跑不起来（详见 §5.10、§5.11）。

**你以后写 Flutter 业务时只需要写 Dart**：像 `example/lib/main.dart` 那样
`FlutterCicadaPlayerFactory().createCicadaPlayer()` + `CicadaTextureView` + 回调，
一行 Java 都不用碰。只有当你要**新增一个平台能力**（比如屏幕旋转锁定、后台播放服务）时，
才需要在第二层加方法并在 Dart 侧加对应的 `invokeMethod`。

---

## 1. 前置条件
| 项 | 本机实际值 | 怎么确认 |
|---|---|---|
| JDK | `C:\Program Files\Java\jdk-21`（`JAVA_HOME` 已设） | `echo %JAVA_HOME%` |
| Android SDK | `D:\Android\Sdk` | `platform/Flutter/example/android/local.properties` 里 `sdk.dir` |
| NDK | `25.2.9519653`（由 `premierlibrary/build.gradle` 的 `ndkVersion` 指定） | `dir D:\Android\Sdk\ndk` |
| Flutter | `D:\flutter`（**不在 PATH**，要写全路径） | `D:\flutter\bin\flutter.bat --version` |
| Gradle 发行版 | `D:\Android\gradle\gradle-8.14.3-bin.zip`（Flutter 3.47 要求 ≥ 8.14） | 三个工程的 `gradle-wrapper.properties` 都用 `file://` 指它 |
| `local.properties` | `example/android/local.properties` 必须有 `sdk.dir` 与 `flutter.sdk` | 内容应为：<br>`sdk.dir=D:\\Android\\Sdk`<br>`flutter.sdk=D:\\flutter` |

---

## 2. 一次性准备：编 aar 并放进 Flutter 插件

### ② 编 aar

```bat
cd /d D:\hilihili\CicadaPlayerNext\platform\Android\source
.\gradlew.bat :premierlibrary:assembleCorePlayerRelease --console=plain
```

产物：`platform\Android\source\premierlibrary\build\outputs\aar\premierlibrary-corePlayer-release.aar`

> 这一步会**同时验证所有 Android-only 的 C++**（`framework/drm/WideVineDrmHandler.cpp`、
> `framework/codec/Android/*`、`ohos` 除外）。为什么重要见 §5.1：**Windows 构建不编这些文件**，
> 所以"Windows 编过"不等于"Android 编过"。

### ③ 拷进插件

```bat
xcopy /y "D:\hilihili\CicadaPlayerNext\platform\Android\source\premierlibrary\build\outputs\aar\premierlibrary-corePlayer-release.aar" ^
         "D:\hilihili\CicadaPlayerNext\platform\Flutter\android\libs\premierlibrary-release.aar"
```

插件侧的 `android/build.gradle` 用通配引入，文件名随意但要保持 `.aar`：

```gradle
implementation fileTree(dir: 'libs', include: ['*.aar'])
```

> `platform/Flutter/android/libs/` 这个目录**原来不存在**，是本次新建的。
> 插件 `.gitignore` 里没有排除 `libs`，所以 aar 会被 git 看到 —— **aar 有 11.5 MB，
> 不要提交**（内核改动后必须重编，提交它只会制造"二进制比源码新/旧"的混乱）。

---

## 3. 编 APK 并安装

```bat
cd /d D:\hilihili\CicadaPlayerNext\platform\Flutter\example

:: ① 拉依赖（改了 pubspec 之后必须跑）
D:\flutter\bin\flutter.bat pub get

:: ② 出 release APK
::    --android-skip-build-dependency-validation 的原因见 §5.7：
::    本工程没有任何 Kotlin 源码，但 Flutter 3.47 仍要求 KGP >= 2.2.20，
::    而本机缓存只有 2.0.21、网速约 100KB/s。Gradle/AGP/JDK 三项我们已手工满足。
::
::    --split-per-abi + --target-platform 是**体积**的关键（见 §3.1）：
::    不写的话会把 arm64 + armeabi-v7a + x86_64 三套都塞进一个 APK（73.3MB）。
D:\flutter\bin\flutter.bat build apk --release ^
    --split-per-abi --target-platform android-arm64,android-arm ^
    --android-skip-build-dependency-validation

:: 产物（每个 ABI 一个）：
::   build\app\outputs\flutter-apk\app-arm64-v8a-release.apk      13.3 MB  ← 真机装这个
::   build\app\outputs\flutter-apk\app-armeabi-v7a-release.apk    12.4 MB
```

### 3.1 体积：73.3 MB → 13.3 MB（比同工程的 Compose 版还小）

先说结论：**74MB 不是内核大，是"三套 ABI + .so 不压缩"堆出来的。**

实测拆解（解 zip 得到的**原始**大小）：

| 项 | 各 ABI 大小 | 说明 |
|---|---|---|
| `libflutter.so` | 11.2 (arm64) / 8.22 (v7a) / **12.45** (x86_64) | **Flutter 引擎**，每个 ABI 一份 |
| `libapp.so` | 4.5 (arm64) / 4.97 (v7a) / **4.69** (x86_64) | Dart AOT 产物，每个 ABI 一份 |
| `libffmpeg.so` | **11.95** (arm64) / 9.87 (v7a) | 内核依赖 |
| `libCicadaPlayer.so` | 2.89 (arm64) / 2.00 (v7a) | 内核 |
| 其余（dex / 资源 / 字体） | ~0.5 | 字体已被 tree-shake（1.6MB → 1.4KB） |

对照同仓库 **Compose 版**（它也用了 ABI splits）：

| | 内核 native | 引擎 | 其它 | 合计 |
|---|---|---|---|---|
| Compose `app-arm64-v8a-release.apk` | libffmpeg 11.95 + libCicadaPlayer 2.89 = **14.85** | 无（Java/Kotlin 不需要引擎 .so） | 8.46（dex + Compose 运行时） | **23.31 MB** |
| Flutter arm64（同 ABI、同样不压缩） | **14.85**（一模一样） | libflutter 11.2 + libapp 4.5 = **15.7** | 0.5 | **≈31 MB** |

也就是说：**内核那一份两边完全一样（14.85 MB），Flutter 多出来的就是引擎的 15.7 MB** ——
这是 Flutter 的固有成本，不是我们代码的问题。剩下那 42 MB 是**多余的 ABI**：
`lib/armeabi-v7a/*`（约 25 MB）+ `lib/x86_64/*`（约 17 MB）。

实测四种配置：

| 配置 | APK |
|---|---|
| 默认（三个 ABI、.so 不压缩） | **73.28 MB** |
| `--target-platform android-arm64` | 42.92 MB ← 只砍掉了引擎/Dart 的 x86_64，**aar 里的 v7a 内核库还在**，见下面的坑 |
| `--split-per-abi --target-platform android-arm64,android-arm` | 42.9 / …（拆分后 31 / 25 MB 级） |
| 上面再叠 `useLegacyPackaging = true`（.so 压缩） | **13.33 MB（arm64）/ 12.36 MB（v7a）** ← 最终结果 |

> ⚠ **一个容易踩的坑**：`flutter build apk --target-platform android-arm64` **只管 Flutter 自己的**
> `libflutter.so` 与 `libapp.so`；**aar 里带的 jniLibs 不受它管**，AGP 照样会把
> `lib/armeabi-v7a/libffmpeg.so` 打进去。所以 42.92 MB 那次里仍然躺着 v7a 的 11.87 MB 内核库。
> 要真正按 ABI 拆分得用 `--split-per-abi`（它同时打开 AGP 的 ABI splits），
> 或者在 gradle 里配 `splits { abi { ... } }` / `defaultConfig.ndk.abiFilters`
>（ComposePlayer 用的正是 `splits`）。

**最终 13.33 MB 的构成**（arm64）：

```
lib/arm64-v8a/libffmpeg.so        11.95 MB -> 4.91 MB（deflate）
lib/arm64-v8a/libflutter.so       11.20 MB -> 5.23 MB
lib/arm64-v8a/libapp.so            4.50 MB -> 1.78 MB
lib/arm64-v8a/libCicadaPlayer.so   2.89 MB -> 0.97 MB
classes.dex                        0.59 MB -> 0.27 MB
```

`useLegacyPackaging = true` 写在 `example/android/app/build.gradle` 的 `packaging` 块里；
代价是**安装时**会把 .so 解压到 `/data`（安装慢一点、多占空间、冷启动可能略慢）。
要回到 AGP 8 的默认（不压缩，更适合上架 AAB）就把那个 `packaging` 块删掉。

> 另外记一条：**x86_64 那份是坏的**（aar 里根本没有 x86_64 的 `libCicadaPlayer.so`），
> 所以默认构建出的 APK 装到 x86_64 模拟器上必崩。用
> `--target-platform android-arm64,android-arm` 把它排除掉，既省 17 MB 又避免一个
> "装上去才知道坏"的变体。

装到设备：

```bat
:: 方式一：flutter 自己装（会自动挑设备）
D:\flutter\bin\flutter.bat install --release

:: 方式二：adb（先确认在列表里）
D:\Android\Sdk\platform-tools\adb.exe devices
D:\Android\Sdk\platform-tools\adb.exe install -r build\app\outputs\flutter-apk\app-release.apk

:: 看日志（Dart 的 print 与原生日志都在里面）
D:\Android\Sdk\platform-tools\adb.exe logcat -s flutter CicadaPlayer AFActiveDecoder
```

调试期想快一点（不混淆、装得快）：

```bat
D:\flutter\bin\flutter.bat build apk --debug
```

---

## 4. 这个 App 是什么：`example/lib/main.dart`（本轮新写的最小测试入口）

它**故意只做"验播放器"这一件事**，屏幕上给出测试需要的全部现场信息：

| 区域 | 内容 |
|---|---|
| 画面 | `CicadaTextureView`（零拷贝纹理）；黑底表示纹理还没就绪或建失败 |
| 状态条 | 纹理是否就绪 / 视频尺寸 / 播放状态 / 进度（**客户端估算**）/ 倍速 / 硬解开关 / 最近错误 |
| 控制 | 播 URL、播本地文件、暂停、继续、停止、硬解开关、循环、倍速（0.5/1/1.5/2） |
| seek | 滑到底松手才 seek（**精准模式**，`seekTo(ms, 1)`） |
| 输入 | 在线地址（默认一个 https 源）、本地路径（默认 `/sdcard/Download/test.mp4`） |
| 日志 | 带时间戳的事件流水：`onPrepared` / `onRenderingStart`（**首帧上屏**）/`onVideoSizeChanged`/`onStateChanged`/`onSeekComplete`/`onError`… |

**验收怎么看**：
1. 点"播 URL"后，日志里应出现 `纹理已创建 textureId=N` → `onPrepared` → `onStateChanged 3 (started)`
   → **`onRenderingStart  ← 首帧上屏`**，且状态条的"视频尺寸"变成真实宽高；
2. 状态条里"状态"若停在 `error`，"最近错误"会给出 `code / extra / msg`；
3. 拖 seek 松手后应出现 `onSeekComplete`，画面继续动；
4. 切"硬解"开关再播，对比 CPU 占用（硬解应明显低）。

> **进度是客户端估算**：Flutter 插件没有暴露"取当前位置"的接口（事件通道里也没有位置事件），
> 所以 App 用"上次同步点 + 过去时间 × 倍速"推算。它只用来判断"在不在走、seek 有没有生效"。

### 4.1 为什么新写入口，而不是修旧的 demo

旧的 `example/lib/page/*` 是 **Dart 2 / Flutter 2 时代**的代码。`flutter analyze` 实测：

| 指标 | 数值 |
|---|---|
| error | **323** |
| 分布 | **23 / 31** 个文件 |
| warning | 54 |
| 最重的几个文件 | `widget/cicada_slider.dart` 45、`model/video_model.dart` 39、`page/player_page.dart` 36、`mytest/model/test_result_model.dart` 35、`page/setting_page.dart` 24 |

错误类型：空安全迁移（`SomeType x` 当可选具名参数用）、Flutter 3 删掉的
`RaisedButton`/`FlatButton`、`class_used_as_mixin`、`return_of_invalid_type`，
以及两个**已经不在 pubspec 里**的依赖（`xml_parser`、`qrcode_reader`）。

那是一整轮迁移的工作量，且与被测主线（零拷贝）无关。所以：
* 旧入口留档在 `example/lib/legacy_main.dart`；
* 旧页面**一个文件都没删**；
* Dart 只编译从入口**可达**的库，所以 APK 能正常构建，旧页面暂时只是"死代码"。

---

## 5. 本轮修掉的问题（4 条环境 + 8 条代码，共 12 条）

> 这些东西是**串行暴露**的：修好前一个才会露出后一个。所以第一次构建会连着失败很多次，
> 下面按"你实际会遇到的顺序"排列。

### 5.1 【代码】块注释里的 `*/` 提前闭合了注释（Android 侧编译失败的第一因）

`framework/drm/WideVineDrmHandler.cpp` 里我写了一句注释：

```
 * ★这三个常量必须与 `platform/Android/**/DrmSessionManager.java` 里的
```

`**/` 里的 `*/` 在 C/C++ 里**就是块注释的结束符**，于是注释在 `Android/**` 处提前结束，
后面那半行变成了代码 → 7 个连环错误：

```
WideVineDrmHandler.cpp:40:49: error: '/*' within block comment [-Werror,-Wcomment]
WideVineDrmHandler.cpp:40:53: error: unknown type name 'DrmSessionManager'
WideVineDrmHandler.cpp:41:70: error: unexpected character <U+2014>
WideVineDrmHandler.cpp:153:22: error: use of undeclared identifier 'kAndroidMediaDrmWidevine'
```

**为什么 Windows 构建没抓到**：`framework/drm/CMakeLists.txt` 里
`WideVineDrmHandler.cpp` 只在 `if (ANDROID)` 下参与编译，**Windows 根本不编这个文件** ——
所以"Windows 全量重编零 error 零 warning"这句话对 Android-only 源码**不成立**。
本轮已修（改成 `platform/Android 下的 DrmSessionManager.java`），并全仓扫描确认没有第二处
（扫描规则：注释续行 `^\s*\*` 里出现 `*/` 且后面还有非空内容）。

### 5.2 【环境】`~/.gradle/gradle.properties` 里的假代理让所有联网构建失败

原文件（有效内容只有 4 行，全错）：

```properties
systemProp.http.proxyHost=http://maven.aliyun.com/repository/public<零宽空格 U+200B>
systemProp.http.proxyPort=80
systemProp.https.proxyHost=https://maven.aliyun.com/repository/public<零宽空格>
systemProp.https.proxyPort=443
```

三个错：
1. `proxyHost` 的语义是**主机名**（Gradle/JVM 交给 `InetAddress.getByName`），这里填的是完整 URL
   ⇒ 每一次联网都变成 `UnknownHostException: https://maven.aliyun.com/repository/public`；
2. 两个值末尾还各有一个**零宽空格 U+200B**（字节 `E2 80 8B`），肉眼看不见；
3. 末尾 4 行 `maven { url '...' }` 是 Gradle DSL，写在 `.properties` 里毫无作用。

阿里的地址是 **Maven 仓库镜像**，不是 HTTP 代理 —— 想用镜像要写进 `init.gradle` 或
`build.gradle` 的 `repositories`（本仓库三个 Android 工程已经这么做了，用 `USE_CHINA_MIRROR` 切）。
**本轮已修**：清掉代理设置，原文件备份为 `gradle.properties.bak-broken`。
修好之后 `services.gradle.org` 立刻可达（实测 DNS + HTTP 200）。

### 5.3 【环境】Gradle 版本：8.13 < Flutter 3.47 要求的 8.14

```
Error: Your project's Gradle version (8.13.0) is lower than Flutter's
       minimum supported version of 8.14.0.
```

本机 `D:\Android\gradle` 只有 8.13 / 8.5 / 9.2.1。**本轮已下载 `gradle-8.14.3-bin.zip`**
放到同一个目录，并把 Flutter 两个 `gradle-wrapper.properties` 都改成它的 `file://` 路径：

```properties
distributionUrl=file\:///D:/Android/gradle/gradle-8.14.3-bin.zip
```

> 为什么不用 9.2.1：AGP 8.7.3 与 KGP 2.0.21 的官方支持范围不含 Gradle 9；
> 8.14.3 同时满足"Flutter ≥ 8.14"与"AGP 8.7.3 / KGP 2.0.21"。

### 5.4 【环境+代码】Flutter 3.29+ 要求**声明式**插件加载

原写法（Flutter 3.29 起被移除）：

```gradle
// example/android/settings.gradle
apply from: "$flutterSdkPath/packages/flutter_tools/gradle/app_plugin_loader.gradle"
```

报错：`You are applying Flutter's app_plugin_loader Gradle plugin imperatively using the
apply script method, which is not possible anymore.`

**本轮已迁移**成官方形态：`settings.gradle` 里
`pluginManagement { includeBuild("$flutterSdkPath/packages/flutter_tools/gradle") }` +
`plugins { id "dev.flutter.flutter-plugin-loader" version "1.0.0"; id "com.android.application" version "8.7.3" apply false; ... }`，
`app/build.gradle` 改用 `plugins { id "com.android.application"; id "dev.flutter.flutter-gradle-plugin" }`，
根 `build.gradle` 删掉与之重复的 `buildscript { classpath 'com.android.tools.build:gradle' }`。
版本号取本机 Gradle 缓存里已有的 AGP **8.7.3** 与 Kotlin **2.0.21**，避免首次构建还要拉新版。

### 5.5 【代码】App 的 `AndroidManifest.xml` 有三个会让构建失败或启动崩溃的问题

| 问题 | 后果 | 修法 |
|---|---|---|
| `package="com.fplayer.flutter_cicadaplayer_example"` | AGP 8 起**移除**了"用清单的 package 设命名空间"，构建直接失败 | 删掉该属性（命名空间由 `app/build.gradle` 的 `namespace` 提供） |
| `android:name="io.flutter.app.FlutterApplication"` | 那是 Android **v1 embedding** 的 Application 类，新引擎已移除 ⇒ 启动 `ClassNotFoundException` | 删掉（v2 embedding 不需要自定义 Application） |
| 缺 `usesCleartextTraffic` | targetSdk ≥ 28 起默认禁止明文 HTTP ⇒ `http://` 源直接连不上 | `<application ... android:usesCleartextTraffic="true">`（测试用；上线要换成 https 或按域白名单） |
| Activity 缺 `android:exported` | targetSdk ≥ 31 起，带 intent-filter 的组件必须显式声明 ⇒ 清单合并直接失败：`android:exported needs to be explicitly specified for element <activity#...MainActivity>` | 给启动 Activity 加 `android:exported="true"`（targetSdk 本轮从 35 抬到 36，所以这条一定会撞上） |

### 5.6 【代码】`libs/` 目录不存在

插件的 `android/build.gradle` 用 `fileTree(dir: 'libs', include: ['*.aar'])` 引 aar，
但 `platform/Flutter/android/libs/` 原来**没有这个目录**（也没有任何 aar）——
即使其它都修好，`flutter build apk` 也会因为找不到 CicadaPlayer 的 Java 类而失败。本轮已建并放入 aar。

### 5.7 【环境+代码】Flutter 3.47 的版本门槛：Gradle ≥ 8.14、AGP ≥ 8.11.1、KGP ≥ 2.2.20

三者是**逐条**拦的，报错过一次才知道下一个：

| 校验 | 报错原文（节选） | 本轮的处理 |
|---|---|---|
| Gradle | `Your project's Gradle version (8.13.0) is lower than Flutter's minimum supported version of 8.14.0.` | 下载 `gradle-8.14.3-bin.zip` 放 `D:\Android\gradle\`，两个 wrapper 都指它（`file://`） |
| AGP | `Your project's Android Gradle Plugin version (8.7.3) is lower than Flutter's minimum supported version of Android Gradle Plugin version 8.11.1.` | `settings.gradle` 的 plugins 块改成 **8.13.0**（本机 Gradle 缓存里已有，不用联网） |
| Kotlin | `Your project's Kotlin version (2.0.21) is lower than Flutter's minimum supported version of 2.2.20.` | **见下**：本工程已无 Kotlin，改成用 `--android-skip-build-dependency-validation` 跳过校验 |

**Kotlin 这一条为什么选择跳过而不是升级**：
1. 本工程**没有任何 Kotlin 源码**（App 与唯一插件都是 Java）；
2. 原来唯一需要 Kotlin 的是 `connectivity_plus` / `path_provider_android` / `sqflite_android`
   这些"为旧 demo 准备的"插件，它们已随最小测试 App 一起从 `pubspec.yaml` 移走（见 §5.8）；
3. KGP 2.2.20 在本机缓存里没有，而本机到 Maven 的实测速度约 **100 KB/s**
   （下载 131 MB 的 Gradle 用了 1336 s），为"一个用不到的插件"下 150 MB 不值。
4. 被这个开关一起跳过的另外三项（JDK / Gradle / AGP）我们已经**手工满足**：
   JDK 21（要求 17+）、Gradle 8.14.3、AGP 8.13.0。

→ 所以 `flutter build apk` 后面要带 `--android-skip-build-dependency-validation`。
将来若要恢复旧 demo 并重新引入 Kotlin 插件，正确做法是**再加回** KGP 2.2.20 并去掉这个开关。

### 5.8 【代码】example 的依赖里有 3 个"拖后腿"的插件

`pubspec.yaml` 里为旧 demo 准备的这几个包，会把 Android 插件模块拖进 Gradle 构建
（Flutter 会为所有已解析的插件 include 其 android 目录，与"Dart 能不能 reach 到"无关），
并因此要求 Kotlin：

```
connectivity_plus / fluttertoast / path_provider_android / sqflite_android
以及 jni / jni_flutter（path_provider_android 的传递依赖，走 native assets）
```

最小测试 App 只用 `flutter` + `flutter_cicadaplayer`，所以本轮把这 6 个包从
`dependencies` 里移走（`dio` / `sqflite` / `path_provider` / `fluttertoast` /
`connectivity_plus` / `cupertino_icons`）。移走后进 Gradle 的 Android 插件只剩
`flutter_cicadaplayer` 一个。恢复办法写在 `pubspec.yaml` 末尾的注释里。

### 5.9 【代码】App 的 `compileSdk` / NDK 与本机 SDK 不匹配

两条都是"模板默认值 vs 本机实装"的冲突：

| 项 | 模板/Flutter 默认 | 本机实装 | 处理 |
|---|---|---|---|
| `compileSdk` / `targetSdk` | 35 | platforms 只有 **android-30/34/36**，**没有 35** | 写成 **36**（AGP 8.13 支持）；否则报 `Failed to find target with hash string 'android-35'` |
| `ndkVersion` | Flutter 3.47 默认 **27.0.12077973** | ndk 只有 **25.2.9519653** | 在 `app/build.gradle` 与插件里都显式钉 `25.2.9519653`；否则报 `Android sdkmanager did not install NDK 27.0.12077973` |

> 为什么钉住 NDK 是安全的：**本 App 与 Flutter 插件都没有任何 native 源码** ——
> CicadaPlayer 的 `.so` 已经在 aar 里，插件也没有 `externalNativeBuild`。
> Flutter 要求的那个 NDK 只是模板默认值，这里用不到。
> （`platform/Android/source` 的 premierlibrary 才真的需要 NDK，它自己声明了 `ndkVersion`。）

### 5.10 【代码】插件里还留着 **Android v1 embedding** 的代码（Flutter 3.29 已删除）

`platform/Flutter/android/src/main/java/...` 三个文件里有 v1 时代的痕迹，编译报 3 个错：

```
error: 找不到符号  类 Registrar
    public FlutterCicadaPlayer(PluginRegistry.Registrar registrar)
error: 对FlutterCicadaPlayer的引用不明确
    new FlutterCicadaPlayer(flutterPluginBinding)   ← 两个构造器都匹配
error: <匿名…$8>不是抽象的, 并且未覆盖OnSubtitleDisplayListener中的抽象方法
    onSubtitleHeader(int,String)
```

本轮的修法：

| 文件 | 改动 |
|---|---|
| `FlutterCicadaPlayerPlugin.java` | 删掉 `import ...PluginRegistry.Registrar`、`registerWith(Registrar)`、字段 `mTextureRegistry`/`mRegistrar`，以及 `obtainPlayer()` 里的 Registrar 分支；纹理注册表一律从 `FlutterPluginBinding` 取 |
| `FlutterCicadaPlayer.java` | 删掉 `FlutterCicadaPlayer(PluginRegistry.Registrar)` 构造器；给匿名 `OnSubtitleDisplayListener` **补上 `onSubtitleHeader(int, String)`**（SDK 后加了这个抽象方法，插件没跟上，所以是"aar 比 Java 源新"导致的必编错误） |
| `PlayerSurface.java` | 见 §5.11 |

> v1 embedding 从 Flutter 3.29 起从引擎移除，`PluginRegistry.Registrar` 这个类**不存在了**，
> 所以任何"兼容 pre-1.12"的分支都必须删掉，不能靠 `@SuppressWarnings` 绕过。

### 5.11 【代码】`PlayerSurface.java` 三处按真实 API 改正（这段代码此前**从未编译过**）

它是随"Flutter 零拷贝"一起写的，而 Flutter Android 那条路一直卡在构建上（缺 aar + Gradle 环境坏），
所以**一次都没被编译**。用 `javap` 从引擎 jar 里 dump 出真实 API 后（`io.flutter.view.TextureRegistry`
及其内部接口），改了三点：

| # | 原来写的 | 真实 API | 后果 |
|---|---|---|---|
| 1 | `mProducer.getId()` | `TextureEntry.id()` | 编译错：找不到符号 方法 getId() |
| 2 | `@Override onSurfaceCreated(Surface)` | 回调里 `onSurfaceCreated()` **不带参数**（且已被 `onSurfaceAvailable()` 取代） | 编译错：方法不会覆盖或实现超类型的方法 |
| 3 | 类实现了 `Callback`，但**没人调 `producer.setCallback(this)`** | 必须显式注册 | **没有编译错，但回调永远不触发** —— 退到后台再回来（GL 上下文重建）时画面可能是死的 |

第 3 条是这次最有价值的发现：它属于"编译能过、跑起来才出问题"的那类。现在构造函数里注册，
并把回调换成当前 API 的 `onSurfaceAvailable()` / `onSurfaceCleanup()`
（旧的 `onSurfaceCreated`/`onSurfaceDestroyed` 已被标记待删除，覆写它们会出 `[removal]` 警告）。

### 5.12 【代码】AGP 8 不允许 library 依赖本地 `.aar`

修完上面之后，卡在最后一步：

```
Execution failed for task ':flutter_cicadaplayer:bundleReleaseAar'.
> Direct local .aar file dependencies are not supported when building an AAR.
  The resulting AAR would be broken because the classes and Android resources from any
  local .aar file dependencies would not be packaged in the resulting AAR.
```

插件是 `com.android.library`，而它 `implementation fileTree(dir:'libs', include:['*.aar'])`，
AGP 8 直接拒绝。标准修法**分成两半**：

| 模块 | 写法 | 为什么 |
|---|---|---|
| 插件 `platform/Flutter/android/build.gradle` | `compileOnly fileTree(dir: 'libs', include: ['*.aar'])` | 只要"编译期能看见 `com.cicada.player.**`"就够了，**不要**打进插件 AAR |
| App `example/android/app/build.gradle` | `implementation files("${rootProject.projectDir}/../../android/libs/premierlibrary-release.aar")` | 由 **App** 负责把 aar（类 + `.so`）打进 APK |

（同一份 aar，不复制第二份；路径从 `example/android` 往上两级到 `platform/Flutter`，再进 `android/libs`。）

### 5.13 【代码】release 一装上就闪退的真凶：R8 把 JNI 要用的类名改掉了

**现象**：`flutter build apk --release` 出来的包，装上桌面能出现图标，点开**立刻闪退**，
Dart 侧一行日志都没有，界面上什么都看不到。

**根因链（每一环都有实测证据）**：

1. **Flutter 的 release 构建默认打开 R8**（这一点最反直觉 —— 我们自己的 gradle 文件里
   一个字都没写 `minifyEnabled`）：
   ```
   packages/flutter_tools/gradle/src/main/kotlin/FlutterPlugin.kt:216
       if (FlutterPluginUtils.shouldShrinkResources(project)) {
           releaseBuildType.isMinifyEnabled = true          // ← 打开代码压缩 + 混淆
           releaseBuildType.isShrinkResources = ...
       }
   FlutterPluginUtils.kt:227  fun shouldShrinkResources(...) = ... // 默认返回 true
   ```
   证据：`build/app/outputs/mapping/release/mapping.txt`（4.4 MB）+ `usage.txt`（1 MB）
   就是 R8 的产物；APK 里还出现了 `res/o-.png` 这种被缩短的资源路径。

2. **CicadaPlayer 的 native 层是按"类名"回调 Java 的**。从 `libCicadaPlayer.so` 里直接
   抽出来的 `FindClass` 字符串共 **22 个**：
   ```
   com/cicada/player/CicadaPlayerGlobalSettings
   com/cicada/player/nativeclass/{CacheConfig,MediaInfo,NativeExternalPlayer,NativePlayerBase,Options,PlayerConfig,TrackInfo}
   com/cicada/player/utils/ass/{AssDialogue,AssHeader,AssStyle,AssUtils}
   com/cicada/player/utils/media/{DrmSessionManager,EncryptionInfo,MediaCodecDecoder,MediaCodecUtils,OutputBufferInfo,SubsampleEncryptionInfo}
   com/cicada/player/utils/{ContentDataSource,DecoderSurfaceTexture,Logger,VsyncTimer}
   ```
   名字被改掉，`FindClass` 就失败 → **JNI 里直接 abort，进程当场死**。

3. **aar 里没有任何 consumer 混淆规则**。核对 `premierlibrary-release.aar` 的全部条目：
   ```
   R.txt / AndroidManifest.xml / classes.jar / META-INF/com/android/build/gradle/aar-metadata.properties
   ```
   —— **没有 `proguard.txt`**。（而且本地 `.aar` 文件依赖本来也不会被 AGP 读取混淆规则。）

4. 于是 R8 把它们改了名甚至删了（摘自那份 mapping.txt）：
   ```
   com.cicada.player.nativeclass.CacheConfig -> f0.a                    ← JNI 用的名字！
   com.cicada.player.CicadaPlayerFactory     -> c0.c
   com.cicada.player.bean.InfoBean           -> d0.a
   com.cicada.player.utils.NativeLoader      -> g0.a
   com.cicada.player.CicadaPlayer$SeekMode   -> R8$$REMOVED$$CLASS$$31  ← 直接被删
   com.cicada.player.bean.InfoCode           -> R8$$REMOVED$$CLASS$$34
   ```
   **22 个 JNI 类名里只剩 3 个活着**（`NativePlayerBase` / `NativeExternalPlayer` / `Logger`）；
   APK 的 dex 里 `Lcom/cicada/player/...` 描述符只有 **3 个**。

**修法**（两处，缺一不可）：

| 文件 | 改动 |
|---|---|
| `platform/Flutter/android/consumer-rules.pro`（**新增**） | `-keep class com.cicada.player.** { *; }` + 22 个类的 `-keepnames` + `-dontwarn`（连成员一起保留：SDK 还用 Gson 反射读写这些类的字段） |
| `platform/Flutter/android/build.gradle` | `defaultConfig { consumerProguardFiles 'consumer-rules.pro' }` —— 这是 Android library 把"消费方必须遵守的混淆规则"传给 App 的标准方式 |

**验证**（重新构建后逐个量的）：

| 检查项 | 修之前 | 修之后 |
|---|---|---|
| dex 里 `Lcom/cicada/player/...` 描述符个数 | **3** | **183** |
| mapping 里 `CacheConfig ->` 右边 | `f0.a` | `com.cicada.player.nativeclass.CacheConfig`（保名） |
| mapping 里 `R8$$REMOVED$$CLASS` 条目数 | 一堆（含 `SeekMode`/`InfoCode`） | **0** |
| `GeneratedPluginRegistrant` 是否在 dex 里 | 在 | 在 |
| APK 体积 | 13.3 MB | 13.4 MB（保留整个 SDK 只花约 0.1 MB） |

> **给以后的人**：任何"用 `flutter build apk --release`，并且插件里有 native 库按类名回调
> Java"的工程都会踩这个坑。两个判断信号：① `mapping.txt` 存在就说明 R8 开了；
> ② 在 [mapping.txt] 里 grep 你的 SDK 包名，右边不是原名的都会被 JNI 找不着。

### 5.14 【代码】桌面图标其实一直没生效：模板图标 + 缺自适应图标

**现状（四个前端，逐个查过）**：

| 前端 | 图标资源 | 结论 |
|---|---|---|
| `platform/QtPlayer` | `assets/appicon/Cicada.ico`（Windows，被 `appQtPlayer.rc` 引用）、`Cicada.icns` + `Cicada.iconset/`（macOS）、`CicadaPlayer-{256,512}.png`（Linux） | **真 logo，设好了** |
| `platform/Android/source/paasApp` | `mipmap-*/ic_launcher{,_round}.png` + `mipmap-anydpi-v26/*.xml` + `drawable/ic_launcher_background.xml` + `drawable-v24/ic_launcher_foreground.xml` + `drawable-nodpi/ic_launcher_foreground.png` | **真 logo，而且是完整的自适应图标**（由 `tools/make_icons/make_icons.py` 生成） |
| `platform/Android/ComposePlayer` | `mipmap-*/ic_launcher{,_round}.webp` + `mipmap-anydpi-v26/*` + `drawable/ic_launcher_{background,foreground}.xml` | **是 Android Studio 的默认模板图标**（青色 `#26A69A` 方块 + 白色网格 + 绿色机器人），**不是**产品 logo |
| `platform/Flutter/example` | 原来是 5 个 442 B ~ 1443 B 的 `ic_launcher.png`（**Flutter 模板的蓝色 logo**），没有自适应图标、没有 round 图标 | **本轮已换成 paasApp 那套真 logo** |

**为什么"只换 mipmap 里的 png 没用"**：API 26+ 的设备优先用 `mipmap-anydpi-v26/ic_launcher.xml`
（adaptive icon），里面的 `background`/`foreground` 指向 `drawable/` 下的资源。
如果只替换旧密度目录里的 png、而 adaptive 的 background 还是模板的青绿方块，桌面上就"看起来没变"。
`paasApp` 的 `ic_launcher_background.xml` 注释里记录的正是这个坑：

> Generated by tools/make_icons/make_icons.py; the previous contents were the Android Studio
> template (a teal #26A69A square plus a white grid), **which is why the launcher icon did not
> change on API 26+ devices.**

**本轮对 Flutter 做的事**：
1. 删掉 Flutter 模板的 5 个 png（否则与新加的 `.webp`/`.png` 同名冲突）；
2. 从 `paasApp` 整套拷来真 logo 的自适应图标资源（背景 shape + 前景矢量/PNG + 各密度 png + anydpi-v26）；
3. 清单里补 `android:roundIcon="@mipmap/ic_launcher_round"`，`android:label` 从字面量改成 `@string/app_name`；
4. 新增 `res/values/strings.xml`，`app_name` = **CicadaPlayer Flutter**（与 Compose 同名会在桌面上分不清，
   要完全一致就改成 `CicadaPlayer`）。

**验证**（对构建出来的 APK 用 `aapt2 dump resources`）：

```
resource 0x7f0a0000 mipmap/ic_launcher
  (mdpi) (hdpi) (xhdpi) (xxhdpi) (xxxhdpi) (anydpi-v26)     ← 6 个 config 齐了
resource 0x7f0a0001 mipmap/ic_launcher_round
  (mdpi) (hdpi) (xhdpi) (xxhdpi) (xxxhdpi) (anydpi-v26)     ← 同样齐全
AndroidManifest: android:icon=@mipmap/ic_launcher  android:roundIcon=@mipmap/ic_launcher_round
```

把 APK 里的 `res/o-.png`（xxxhdpi ic_launcher）解出来看，是 CicadaPlayer 那个青色播放键/蝉翼 logo。

> **ComposePlayer 的图标还欠着**：它现在用的还是 Android Studio 默认模板。修法与 Flutter 这边一样
> （把 `paasApp` 那套拷过去），但那是另一个前端，本轮没动。更彻底的做法的给
> `tools/make_icons/make_icons.py` 的 `build_plan()` 再加一个 Android 输出目标，
> 一条命令把 paasApp / ComposePlayer / Flutter 三处都刷成同一份 logo。

### 5.15 【代码】Dart 在原生播放器还不存在时就订阅事件通道 ⇒ `MissingPluginException`

**现象**（闪退修好之后的下一个错误，界面顶部那条红条就是它）：

```
FlutterError: MissingPluginException(No implementation found for method listen
              on channel flutter_cicadaplayer_event)
```

**根因**：原生侧三条通道的 handler **不是一起注册的**。

| 通道 | 何时注册 handler |
|---|---|
| `plugins.flutter_cicadaplayer_factory`（工厂） | 插件挂载时（`FlutterCicadaPlayerPlugin.onAttachedToEngine`） |
| `flutter_cicadaplayer`（播放器） | **原生播放器对象构造时**（`FlutterCicadaPlayer` 构造函数里的 `setMethodCallHandler`） |
| `flutter_cicadaplayer_event`（事件） | 同上（同一构造函数里的 `setStreamHandler`） |

而原生播放器只在**工厂通道**的 `createCicadaPlayer` / `createTexture` 里被建出来
（`FlutterCicadaPlayerPlugin.obtainPlayer()`）。原来的 Dart 构造函数里是：

```dart
eventChannel.receiveBroadcastStream().listen(...);   // ← 先订阅（此时原生侧还没有 handler）
ensureTexture().then(...);                          // ← 后建纹理（这一步才把播放器建出来）
```

**顺序反了**。更要紧的是**它不会自愈**：`receiveBroadcastStream` 的 `listen` 只往原生发一次，
抛掉之后这条流不会再发第二次 ⇒ `onPrepared` / `onRenderingStart` 等事件一个都收不到
（表现是画面一直黑着、日志一片安静，而错误提示只有顶部那一条）。

> 为什么这条错误不走 Dart 的 `onError`：`EventChannel` 的实现在 `onListen` 里是
> `FlutterError.reportError(...)`，所以它进的是**全局错误处理器**，界面上就是那条红条。

**修法**（`platform/Flutter/lib/flutter_cicadaplayer.dart`）：把订阅移到
`ensureTexture()` **成功之后**。`createTexture` 走的是工厂通道，它返回时原生播放器一定已经
构造完、两条通道的 handler 都已就绪：

```dart
ensureTexture().then(_listenEventChannel, onError: _swallowTextureError);

void _listenEventChannel(int textureId) {
  eventChannel.receiveBroadcastStream().listen(_onEvent, onError: _onError);
}
```

（顺手删掉了不再使用的 `_swallowTextureResult`。）

**由此得出一条调用约定**：**播放器通道（`flutter_cicadaplayer`）上的任何方法，
都必须等纹理就绪之后再调** —— 也就是在 `CicadaTextureView` 的 `onCreated` 回调之后。
example 的 `main.dart` 就是这么写的（`_startTicker`、`setUrl`、`prepare` 都在 `onCreated`
之后或按钮点击时触发）。反过来在 `createCicadaPlayer()` 之后立刻 `setUrl` 就会重现这个异常。

### 5.16 【代码】插件有 26 个方法**不回包** ⇒ Dart 的 `await` 永远不返回（"点了播放没反应"）

**现象**（本条与 §5.15 是先后两个独立问题）：顶部没有红条、也没有崩溃，点"播 URL"
**什么都不发生**，状态条一直是 `状态: 未创建`、纹理那一格也没变化。看起来像"点击没生效"。

**根因**：原生 `FlutterCicadaPlayer.dispatchMethodCall` 里共 **45 个 `case`，只有 18 个**
调了 `result.success(...)`。其余（`setUrl` / `prepare` / `play` / `stop` / `pause` /
`setLoop` / `seekTo` …）执行完动作就 `break`：

```java
case "setUrl":
    String url = methodCall.arguments.toString();
    setDataSource(url);          // ← 干完活就走了
    break;                       // ← 没有 result.success()，Dart 那边永远等不到回包
```

而 Dart 侧这些方法都是 `await channel.invokeMethod(...)` ⇒ **Future 永远不完成**：

```dart
await _player.stop();        // ← 卡在这里，下面的全都没执行
await _player.setUrl(url);
await _player.prepare();
```

所以 `setUrl` / `prepare` **根本没被调用过**，原生自然一个事件都不发 —— 与"状态未创建"
（`onStateChanged` 从没来过）完全吻合。

> **为什么这个 bug 能躺这么久**：老的 example 是 fire-and-forget（`player.setUrl(url)`
> 不 await），Future 挂着也没人管，看起来"能用"。直到有人按正常写法 `await` 才暴露出来 ——
> 也就是说，**任何按文档正常 await 这个插件的 Flutter 应用都会踩到**。

**修法**（`FlutterCicadaPlayer.java`）：不去改 45 个 case，而是在 `onMethodCall` 外面套一层
`ReplyingResult`（记录"原生回过包没有"），末尾统一补一个 `success(null)`：

```java
ReplyingResult replying = new ReplyingResult(result);
try {
    dispatchMethodCall(methodCall, replying);
} catch (Throwable t) { ...; replying.error(...); return; }
if (!replying.hasReplied()) {
    replying.success(null);      // 只执行动作的分支
}
```

**顺带修的第二个隐患**：`mEventSink` 原来只在"尺寸回调"那一处判空，其余 ~18 处直接
`.success(...)`。原生播放器在 `createTexture` 里就构造好并挂上监听了，而 Dart 要等
`createTexture` 的 Future 返回后才 listen —— 这中间来的事件（状态变化、尺寸）会让
原生回调线程抛空指针（JNI 回调里抛异常极难查）。现在所有回调统一走
`sendEvent(Map)`，内部判空。

**验证**：

* `flutter analyze`：No issues found；
* 出包成功（13.5 MB / 12.5 MB）；
* 新代码确实进了包 —— R8 会把私有类/方法改名，所以在 `mapping.txt` 左侧核对原始名：
  ```
  com.fplayer.flutter_cicadaplayer.FlutterCicadaPlayer$ReplyingResult -> c0.m:
  1:4:void sendEvent(java.util.Map):359:359 -> a
  ```

**example 里同时加了三个"下次能自己看出来"的东西**（都是这次踩坑的产物）：

1. **每一步原生调用前后各写一行日志**：`→ setUrl` / `← setUrl ok`。
   只有 `→` 没有 `←`，就是那次调用没回包 —— 一眼定位，不用猜。
2. **状态条上加了 `事件: N`**（原生事件累计数）。点播放后它一直是 0，
   就说明原生一个事件都没送来（而不是"解码慢"）。
3. **纹理那格会显示"就绪/创建中/失败"** —— `CicadaTextureView` 把建纹理失败降级成黑屏，
   光看黑屏分不清是没纹理还是没解码。

---

## 6. 故障速查

| 现象 | 原因 | 处理 |
|---|---|---|
| `Failed to parse host https://maven.aliyun.com/...` / `UnknownHostException` | `~/.gradle/gradle.properties` 里的假代理 | 见 §5.2（已修；升级/换机后要再看一眼） |
| `Your project's Gradle version (X) is lower than Flutter's minimum supported version of 8.14.0` | wrapper 指向旧 Gradle | 见 §5.3 |
| `app_plugin_loader ... not possible anymore` | 命令式插件加载 | 见 §5.4 |
| `package="..." found in source AndroidManifest.xml` | AGP 8 移除该写法 | 见 §5.5 |
| 启动即崩、`ClassNotFoundException: io.flutter.app.FlutterApplication` | v1 embedding 类 | 见 §5.5 |
| `http://` 源报 `CLEARTEXT communication not permitted` | 缺 usesCleartextTraffic | 见 §5.5 |
| APK 起来了但播的是**旧内核** | 忘了重编 aar（§2） | 重跑 ②③ 再 build |
| `flutter pub get` 解析失败 | example 里引用了已移除的三个包 | 见 §4.1（旧页面已不参与编译，但**不要**把它们 import 回入口） |
| 改了 `pubspec.yaml` 后构建报缺包 | 没跑 `pub get` | `D:\flutter\bin\flutter.bat pub get` |
| `lower than Flutter's minimum supported version of ...` | Gradle / AGP / KGP 版本门槛 | 见 §5.7（Gradle 8.14.3 + AGP 8.13.0；Kotlin 用 `--android-skip-build-dependency-validation` 跳过） |
| `Failed to find target with hash string 'android-35'` | SDK 里没装该 platform | 见 §5.9（改用 36） |
| `Android sdkmanager did not install NDK 27.0.12077973` | Flutter 默认 NDK 与本机不符 | 见 §5.9（显式钉 `ndkVersion`） |
| `error: 找不到符号 类 Registrar` / `引用不明确` | 插件里还有 v1 embedding 代码 | 见 §5.10 |
| `不是抽象的, 并且未覆盖 ... onSubtitleHeader` | aar 比插件 Java 源新（SDK 加了抽象方法） | 见 §5.10（补实现） |
| `Direct local .aar file dependencies are not supported when building an AAR` | library 模块不能依赖本地 aar | 见 §5.12（插件 `compileOnly` + App `implementation`） |
| `android:exported needs to be explicitly specified` | targetSdk ≥ 31 的要求 | 见 §5.5（给带 intent-filter 的 Activity 加 `android:exported="true"`） |
| **装上一打开就闪退**，Dart 侧没有任何日志 | R8 把 JNI 要用的类名改了（release 默认开 R8，aar 又没有 consumer 规则） | 见 §5.13 |
| 桌面图标不是产品 logo（或换了 png 也没变） | 模板图标 + 缺/错的 adaptive icon | 见 §5.14 |
| 顶部红条：`MissingPluginException ... listen on channel flutter_cicadaplayer_event` | Dart 在原生播放器建出来之前就订阅了事件通道 | 见 §5.15 |
| 界面黑着、日志一片安静（没有 onPrepared） | 同上的"listen 只发一次、抛了不再重发" | 见 §5.15 |
| 顶部红条出现任意 Dart 异常 | `gFatalError` 的显示（release 下默认只有 logcat 有） | `adb logcat -s flutter` 看完整栈；平时它是黑屏 |
| **点"播 URL"什么都不发生**、没红条、状态一直"未创建" | 插件大多数方法**不回包**，Dart 的 await 卡死 | 见 §5.16（已修，并用 `→/←` 日志把"卡在哪一步"显示出来） |
| 状态条 `事件: 0` 一直不动 | 原生一个事件都没发过来（不是解码慢） | 先看 §5.15 / §5.16，再用 `adb logcat -s FlutterCicadaPlayer` |
| 想播本地文件但没有文件选择器 | 本工程故意不引插件（会带 Kotlin 依赖） | 用"扫描文件"按钮（走应用自己的外部目录，免权限）：<br>`adb push test.mp4 /sdcard/Android/data/com.fplayer.flutter_cicadaplayer_example/files/` |

---

## 7. 仍然欠的账（不影响本次测试）

1. **旧 demo 的 Dart 3 迁移**：323 个 error / 23 个文件（见 §4.1）。
   需要把 `xml_parser` → `package:xml`、`qrcode_reader` → `mobile_scanner`、
   下拉刷新 → `RefreshIndicator`、`RaisedButton` → `ElevatedButton`，
   然后 `main.dart` 才能指回 `page/home_page.dart`；同时把 §5.8 移走的依赖加回来，
   并按 §5.7 的处理方式重新引入 KGP 2.2.20 然后去掉跳过校验的开关。
2. **iOS 侧的 Flutter**：插件有 `ios/Classes/FlutterCicadaPlayerTexture.{h,m}`，
   但需要 macOS + Xcode 生成预编译 framework（本机 Windows 做不了）。
3. **aar 的产物管理**：目前是"手工拷贝"。真正上流水线时应该由 CI 在编内核的同时产出并归档 aar。
4. **x86_64 模拟器不能用**：aar 里只有 arm64-v8a 与 armeabi-v7a 的 `.so`（见 §3 的核对输出）。
   要跑模拟器就得让 Android 侧也产出 x86_64 的 aar。
5. **`~/.gradle/gradle.properties` 是机器级的**：本轮修的是这台机器上的那份
   （原件备份为 `gradle.properties.bak-broken`）。换机器/重装后要再看一眼有没有被别的工具写回坏代理。
