// ===========================================================================
// :cicadaplayer —— 内核模块（产出 releaseLibs/CicadaPlayer-4.7.0-full.aar）
//
// 这个模块照老工程 platform/Android/source/premierlibrary 的做法来：
//   · AGP 官方 externalNativeBuild → IDE 里能看到并跳转内核的 framework / mediaPlayer / plugin，
//     C++ 补全和断点都在；原生编译是增量的（改 Kotlin 不会触发 C++ 编译，Sync 从不编译）；
//   · jniLibs 直接指向内核的 external/install/ffmpeg/Android（老工程同款，libffmpeg.so 自动打包）；
//   · ndk abiFilters 只有 armeabi-v7a / arm64-v8a；
//   · buildTypes 里 debugSymbolLevel = NONE（老工程同款，避免 .so 带全量调试信息把 APK 撑大）；
//   · assembleRelease 之后把 aar 复制成 CicadaPlayer-4.7.0-full.aar。
//
// 与老工程只有两处必要差异：
//   1. CMake 入口在 src/main/cpp/CMakeLists.txt（AGP 约定位置，老工程在模块根）。所以
//      TOPDIR（内核根）和 JNI 目录由那个 CMakeLists 用相对位置自己算，Gradle 不再传
//      -DCICADA_ROOT / -DCICADA_JNI_DIR；
//   2. 内核不在本工程里，所以内核位置由下面的 cicadaRoot 解析。
//
// 【整棵 CicadaPlayerNext 不会再被拖进工程视图】：内核 framework/Android.cmake 里那条
//   `${PROJECT_SOURCE_DIR}/../`（在 mediaPlayer 作用域 = 整个内核根）已删，详见那个文件的注释。
// ===========================================================================
plugins {
    alias(libs.plugins.android.library)
}

/**
 * 内核 CicadaPlayerNext 的位置（自动认两种布局，谁先存在用谁）：
 *
 *   ① 推荐：本工程与内核**并排**，工程目录不在内核树里面
 *        D:\hilihili\JetpackComposePlayer
 *        D:\hilihili\CicadaPlayerNext
 *   ② 旧布局：本工程还在 CicadaPlayerNext\platform\JetpackComposePlayer 下（../..）
 */
val cicadaRoot: File = listOf(
    rootProject.file("../CicadaPlayerNext"),   // ① 并排
    rootProject.file("../..")                  // ② 还在 platform 下
).firstOrNull { File(it, "mediaPlayer").isDirectory && File(it, "framework").isDirectory }
    ?: throw GradleException(
        "找不到内核 CicadaPlayerNext：请把本工程放在 D:\\hilihili\\JetpackComposePlayer（与 CicadaPlayerNext 同级）"
    )

/**
 * 内核依赖的预编译库目录（每个 ABI 一份 libffmpeg.so）。
 *   · 编内核时把该目录 -D 传给 CMake；
 *   · 打包用的那一份已复制到模块内 src/main/jniLibs/<abi>/（官方默认位置），不从这里取。
 */
val ffmpegAndroid = File(cicadaRoot, "external/install/ffmpeg/Android")

/**
 * 传给 CMake 的路径用正斜杠。
 * Windows 的 `D:\hilihili\...` 里 `\h` 会被 CMake 当转义序列，报
 *   Syntax error in cmake code when parsing string ... Invalid character escape '\h'
 */
fun cmakePath(file: File): String = file.absolutePath.replace('\\', '/')

android {
    namespace = "com.cicada.player"
    compileSdk = 36
    buildToolsVersion = "36.1.0"
    ndkVersion = "25.2.9519653"

    defaultConfig {
        minSdk = 24

        externalNativeBuild {
            cmake {
                // 只传内核依赖目录 + 工具链/STL；内核根、JNI 目录由 src/main/cpp/CMakeLists.txt 自己算
                arguments.addAll(
                    listOf(
                        "-DANDROID_TOOLCHAIN=clang",
                        "-DANDROID_STL=c++_static",
                        "-DFFMPEG_INSTALL_DIR_ANDROID=${cmakePath(ffmpegAndroid)}/",
                        "-DEXTERN_INSTALL_DIR_ANDROID=${cmakePath(File(cicadaRoot, "external/install"))}/"
                    )
                )
                cppFlags.add("-std=c++11")
            }
        }

        ndk {
            abiFilters.addAll(listOf("armeabi-v7a", "arm64-v8a"))
        }
    }

    externalNativeBuild {
        cmake {
            // AGP 官方位置：src/main/cpp/CMakeLists.txt（AS 的 Link C++ Project 生成的就是这个）
            path = file("src/main/cpp/CMakeLists.txt")
            /*
             * 【不要钉死 CMake 版本】写了 version = "3.22.1" 的话，SDK Manager 里没装这个版本就会
             * 直接 "CMake 3.22.1 not found"。交给 AGP 用 SDK 里已装的 CMake 即可。
             */
        }
    }

    /*
     * 预编译库走官方默认位置：src/main/jniLibs/<abi>/libffmpeg.so
     * 所以这里**不写**任何 sourceSets / jniLibs.srcDirs（老工程那种"指向模块外目录"的写法
     * 会让 AS 多挂一个外部内容根；现在不需要了）。
     */

    buildTypes {
        debug {
            ndk { debugSymbolLevel = "NONE" }
        }
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android.txt"), "proguard-rules.pro")
            ndk { debugSymbolLevel = "NONE" }
        }
    }

    packaging {
        jniLibs { useLegacyPackaging = false }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    lint { abortOnError = false }
}

// ---------------------------------------------------------------------------
// 老工程同款：assembleRelease 之后把 aar 复制成 releaseLibs/CicadaPlayer-4.7.0-full.aar
//   ./gradlew :cicadaplayer:assembleRelease
// ---------------------------------------------------------------------------
val releaseLibsDir = rootProject.file("releaseLibs")

tasks.matching { it.name == "assembleRelease" }.configureEach {
    doLast {
        val aar = layout.buildDirectory.file("outputs/aar/cicadaplayer-release.aar").get().asFile
        if (aar.exists()) {
            releaseLibsDir.mkdirs()
            val out = File(releaseLibsDir, "CicadaPlayer-4.7.0-full.aar")
            aar.copyTo(out, overwrite = true)
            println("[cicadaplayer] AAR -> $out")
        } else {
            println("[cicadaplayer] 没找到 $aar")
        }
    }
}
