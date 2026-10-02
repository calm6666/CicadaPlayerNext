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
D:\flutter\bin\flutter.bat build apk --release --android-skip-build-dependency-validation

:: 产物（两处是同一个文件）：
::   build\app\outputs\flutter-apk\app-release.apk
::   build\app\outputs\apk\release\app-release.apk
```

**实测结果**：`✓ Built build\app\outputs\flutter-apk\app-release.apk (73.3MB)`，
内容核对（`tar -tf`）：

```
lib/arm64-v8a/libCicadaPlayer.so   lib/arm64-v8a/libffmpeg.so   lib/arm64-v8a/libapp.so   lib/arm64-v8a/libflutter.so
lib/armeabi-v7a/libCicadaPlayer.so lib/armeabi-v7a/libffmpeg.so lib/armeabi-v7a/libapp.so lib/armeabi-v7a/libflutter.so
classes.dex
```

> ⚠ `libCicadaPlayer.so` / `libffmpeg.so` **只有 arm64-v8a 与 armeabi-v7a**（aar 里就没有 x86_64）。
> 所以要装在**真机**上测；x86_64 模拟器会因为缺 .so 起不来。
>
> 73.3 MB 偏大是正常的：两个 ABI 的 libffmpeg（12.5 MB + 10.3 MB）加两个 ABI 的
> libCicadaPlayer 与 libflutter 都在里面。只想测一个 ABI 时用
> `flutter build apk --release --target-platform android-arm64` 可以砍掉一半。

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
