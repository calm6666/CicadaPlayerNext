# CicadaPlayer 的 Android 侧混淆规则（consumer rules）——**必须保留**
#
# 【为什么必须有这个文件】
# Flutter 的 Gradle 插件在 release 构建里**默认打开 R8**：
#   packages/flutter_tools/gradle/src/main/kotlin/FlutterPlugin.kt:216
#       if (FlutterPluginUtils.shouldShrinkResources(project)) {
#           releaseBuildType.isMinifyEnabled = true          // ← 打开代码压缩+混淆
#           releaseBuildType.isShrinkResources = ...          // ← 打开资源压缩
#       }
#   而 shouldShrinkResources() 的默认返回值就是 true（FlutterPluginUtils.kt:227）。
# 也就是说：**只要用 flutter build apk --release，就会有 R8 改名**，不需要谁去配 minifyEnabled。
#
# 而 CicadaPlayer 的 native 层是**按类名**回调 Java 的：
#   libCicadaPlayer.so 里写死了 22 个 FindClass 字符串，例如
#       com/cicada/player/nativeclass/MediaInfo
#       com/cicada/player/nativeclass/PlayerConfig
#       com/cicada/player/nativeclass/CacheConfig
#       com/cicada/player/utils/media/MediaCodecDecoder
#       com/cicada/player/utils/media/DrmSessionManager
#       com/cicada/player/utils/VsyncTimer
#       com/cicada/player/CicadaPlayerGlobalSettings   …… 等 22 个
# 名字一旦被 R8 改掉或删掉，native 侧 FindClass 就失败，**进程直接死**（表现为"装上一打开就闪退"，
# 界面上什么都不显示，Dart 侧一行日志都来不及打）。
#
# 实测过的破坏（一份 R8 mapping.txt 里摘出来的）：
#   com.cicada.player.nativeclass.CacheConfig  -> f0.a            （JNI 需要这个名字！）
#   com.cicada.player.CicadaPlayerFactory      -> c0.c
#   com.cicada.player.bean.InfoBean            -> d0.a
#   com.cicada.player.utils.NativeLoader       -> g0.a
#   com.cicada.player.CicadaPlayer$SeekMode    -> R8$$REMOVED$$CLASS$$31   （直接被删）
#   com.cicada.player.bean.InfoCode            -> R8$$REMOVED$$CLASS$$34
# 22 个 JNI 类名里只剩 3 个（NativePlayerBase / NativeExternalPlayer / Logger）活着。
#
# 【为什么规则要放在这里，而不是 aar 里】
# 已核对 premierlibrary-release.aar 的全部条目：只有 R.txt / AndroidManifest.xml / classes.jar /
# aar-metadata.properties，**没有 proguard.txt（consumer rules）**。
# 一个 Android library 如果要以本地 .aar 文件的形式被依赖，它的 proguard.txt 本来也不会被
# AGP 读取；所以规则只能由**插件**（本模块）通过 consumerProguardFiles 提供给 App，
# 或者在 App 自己的 proguard-rules.pro 里写一份（example 里也放了一份作为兜底）。
#
# 【为什么是 -keep class ... { *; } 而不是只 keep 类名】
# 除了 JNI，SDK 还用 Gson 反射读写 nativeclass.** / bean.** 的**字段**（设备信息、配置、MediaInfo
# 等）。字段名被改掉同样会静默出错，所以连成员一起保留。SDK 整体不到 200KB 字节码，
# 全量保留的代价很小，换来的是"不会因为混淆而出玄学问题"。

-keep class com.cicada.player.** { *; }

# 这些类是被 JNI 或反射按名字使用的，同样的道理再显式列一遍（可读性：
# 出问题时一眼能看出到底哪些名字不能动）。
-keepnames class com.cicada.player.CicadaPlayerGlobalSettings
-keepnames class com.cicada.player.nativeclass.CacheConfig
-keepnames class com.cicada.player.nativeclass.MediaInfo
-keepnames class com.cicada.player.nativeclass.NativeExternalPlayer
-keepnames class com.cicada.player.nativeclass.NativePlayerBase
-keepnames class com.cicada.player.nativeclass.Options
-keepnames class com.cicada.player.nativeclass.PlayerConfig
-keepnames class com.cicada.player.nativeclass.TrackInfo
-keepnames class com.cicada.player.utils.ass.AssDialogue
-keepnames class com.cicada.player.utils.ass.AssHeader
-keepnames class com.cicada.player.utils.ass.AssStyle
-keepnames class com.cicada.player.utils.ass.AssUtils
-keepnames class com.cicada.player.utils.ContentDataSource
-keepnames class com.cicada.player.utils.DecoderSurfaceTexture
-keepnames class com.cicada.player.utils.Logger
-keepnames class com.cicada.player.utils.media.DrmSessionManager
-keepnames class com.cicada.player.utils.media.EncryptionInfo
-keepnames class com.cicada.player.utils.media.MediaCodecDecoder
-keepnames class com.cicada.player.utils.media.MediaCodecUtils
-keepnames class com.cicada.player.utils.media.OutputBufferInfo
-keepnames class com.cicada.player.utils.media.SubsampleEncryptionInfo
-keepnames class com.cicada.player.utils.VsyncTimer

# SDK 里有些类只在 native 侧用到，R8 静态看不到引用会报"找不到"警告；按名字使用的东西
# 不能算作可安全删除。
-dontwarn com.cicada.player.**
