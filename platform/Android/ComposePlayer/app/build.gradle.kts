import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

/*
 * ============ release 签名配置（从工程根的 keystore.properties 读）============
 *
 * 规则：**文件不存在（或没填 storeFile）就完全跳过** —— release 仍按今天的方式产出
 * `-unsigned` 包，debug 流程一字不变。存在就挂上签名，`assembleRelease` 直接产出可安装包。
 *
 * keystore.properties 内容（放在 platform/Android/ComposePlayer/ 下，**不要入库**）：
 *   storeFile=D:/hilihili/keys/cicada-release.jks
 *   storePassword=你的库口令
 *   keyAlias=cicada
 *   keyPassword=你的库口令
 *
 * 生成密钥库（一次性；AS 自带 JDK 里就有 keytool）：
 *   & "C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe" -genkeypair -v `
 *     -keystore "D:\hilihili\keys\cicada-release.jks" -alias cicada `
 *     -keyalg RSA -keysize 2048 -sigalg SHA256withRSA -validity 10000 `
 *     -storetype PKCS12 -dname "CN=Cicada Player, OU=Dev, O=Hilihili, L=Beijing, ST=Beijing, C=CN"
 *
 * 注意：PKCS12 下 storePassword 与 keyPassword 必须相同。
 */
/*
 * 读 keystore.properties。注意用 Properties 的**简单名**：Kotlin DSL 里 `java` 会被
 * AGP 生成的 `java` 扩展访问器遮蔽，直接写 `java.util.Properties` 会编不过
 * （Unresolved reference: util / load），所以文件头已经 `import java.util.Properties`。
 */
val keystorePropertiesFile = rootProject.file("keystore.properties")
val keystoreProperties = Properties()
if (keystorePropertiesFile.exists()) {
    keystorePropertiesFile.inputStream().use { keystoreProperties.load(it) }
}
val hasReleaseKeystore = !keystoreProperties.getProperty("storeFile").isNullOrBlank()
if (hasReleaseKeystore) {
    val missing = listOf("storePassword", "keyAlias", "keyPassword")
        .filter { keystoreProperties.getProperty(it).isNullOrBlank() }
    if (missing.isNotEmpty()) {
        println("[signing] keystore.properties 缺少字段 ${missing.joinToString()} —— release 签名会失败")
    }
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

    /*
     * 签名配置：只有 keystore.properties 真的填了 storeFile 才创建（否则不创建、
     * release 保持"未签名"的旧行为）。
     */
    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                storeFile = file(keystoreProperties.getProperty("storeFile"))
                storePassword = keystoreProperties.getProperty("storePassword")
                keyAlias = keystoreProperties.getProperty("keyAlias")
                keyPassword = keystoreProperties.getProperty("keyPassword")
                /* minSdk 24 ⇒ v2 足够；v1 一起开是为了兼容个别老设备/第三方校验工具 */
                enableV1Signing = true
                enableV2Signing = true
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            /* 有签名就挂上；没有 ⇒ 与今天完全一致（产出 -unsigned，装不上但能编过） */
            if (hasReleaseKeystore) {
                signingConfig = signingConfigs.getByName("release")
            }
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