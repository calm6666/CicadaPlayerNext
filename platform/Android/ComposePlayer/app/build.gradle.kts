plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

android {
    namespace = "com.cicada.player.compose"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.cicada.player.compose"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        /*
         * 【这里不要写 ndk.abiFilters】AGP 不允许"ndk.abiFilters"和下面的"splits.abi.include"
         * 同时出现（报 Conflicting configuration ... cannot be present when splits abi filters are set）。
         * "只出 arm64-v8a + armeabi-v7a、不要 x86"由下面的 splits 保证；
         * :cicadaplayer 那个 AAR 本身也只编这两个 ABI（它的 abiFilters 在它自己的 build.gradle.kts 里）。
         */
    }

    /*
     * ABI 分包：打两个独立 APK，一个 64 位一个 32 位，不出 universal 包
     *   产物：app/build/outputs/apk/<buildType>/app-arm64-v8a-<buildType>.apk
     *        app/build/outputs/apk/<buildType>/app-armeabi-v7a-<buildType>.apk
     * AS 里 Run/Debug 会按设备 ABI 自动挑对应的那个。
     */
    splits {
        abi {
            isEnable = true
            reset()
            include("arm64-v8a", "armeabi-v7a")
            isUniversalApk = false
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    kotlinOptions {
        jvmTarget = "11"
    }
    buildFeatures {
        compose = true
    }
    buildToolsVersion = "36.1.0"
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.graphics)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)

    // 官方导航（Compose Navigation）：单 Activity + NavHost
    implementation(libs.androidx.navigation.compose)

    // ============ 播放内核：本工程自己的 CicadaPlayer（CicadaPlayerNext 的 C++ 核心）============
    // 【已删除全部 ExoPlayer/Media3 依赖】media3-exoplayer / -ui / -common / -exoplayer-dash /
    // -exoplayer-hls / -datasource-okhttp 一个都不留。
    // 播放/解复用/解码/渲染全部走 :cicadaplayer（与 Qt 播放器同一个内核：
    // 硬解 enableHardwareDecoder、零拷贝 setSurface、DASH/HLS/FLV、播放对象 setDataSource(JSONObject)、
    // 无缝切档 selectTrack），接口见 com.cicada.player.CicadaPlayer。
    implementation(project(":cicadaplayer"))

    // Lottie：播放/暂停 + 进度条 dot（三颗图标，json 从 Qt 工程复制）
    implementation(libs.lottie)
    implementation(libs.lottie.compose)

    // ViewModel 和生命周期
    implementation(libs.androidx.lifecycle.viewmodel.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    implementation(libs.androidx.lifecycle.runtime.compose)


    // ============ 上传功能新增依赖 ============
    // OkHttp：HTTP 客户端（含预签名 URL 的 PUT 直传与进度回调）
    implementation(libs.okhttp)
    // Gson：JSON 解析（配合自研驼峰↔下划线转换器处理 Go 后端信封）
    implementation(libs.gson)
    // 协程：并发分片上传、暂停/恢复/取消的调度基础
    implementation(libs.kotlinx.coroutines.android)
    // Coil：封面等图片预览的异步加载
    implementation(libs.coil.compose)

    // Hilt
    implementation(libs.hilt.navigation.compose)
    implementation(libs.hilt.android)
    // implementation(libs.hilt.compiler)


    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    debugImplementation(libs.androidx.compose.ui.tooling)
    debugImplementation(libs.androidx.compose.ui.test.manifest)
}