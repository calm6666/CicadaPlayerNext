pluginManagement {
    repositories {
        // 阿里云镜像
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/gradle-plugin") }
        maven { url = uri("https://maven.aliyun.com/repository/central") }

        // 华为云镜像
        maven { url = uri("https://repo.huaweicloud.com/repository/maven/") }

        // 腾讯云镜像
        maven { url = uri("https://mirrors.cloud.tencent.com/nexus/repository/maven-public/") }

        // 网易镜像
        maven { url = uri("https://mirrors.163.com/maven/repository/maven-public/") }

        // 首都在线
        maven { url = uri("https://maven.oscs.oschina.net/content/groups/public/") }

        // 保留中央仓库
        mavenCentral()

        // 保留Google仓库(备选)
        google {
            content {
                includeGroupByRegex("com\\.android.*")
                includeGroupByRegex("com\\.google.*")
                includeGroupByRegex("androidx.*")
            }
        }
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        // 阿里云镜像
        maven { url = uri("https://maven.aliyun.com/repository/public") }
        maven { url = uri("https://maven.aliyun.com/repository/google") }
        maven { url = uri("https://maven.aliyun.com/repository/central") }

        // 华为云镜像
        maven { url = uri("https://repo.huaweicloud.com/repository/maven/") }

        // 腾讯云镜像
        maven { url = uri("https://mirrors.cloud.tencent.com/nexus/repository/maven-public/") }

        // 网易镜像
        maven { url = uri("https://mirrors.163.com/maven/repository/maven-public/") }

        // 首都在线
        maven { url = uri("https://maven.oscs.oschina.net/content/groups/public/") }

        google()
        mavenCentral()
    }
}

rootProject.name = "CicadaPlayer"

// 内核模块：用 NDK/CMake 编译 CicadaPlayerNext 的 C++ 核心（与 Qt 播放器同一个内核），
// JNI 经 com.cicada.player.* 暴露；assembleRelease 产出 releaseLibs/CicadaPlayer-4.7.0-full.aar。
// App 层不使用 ExoPlayer/Media3。
include(":cicadaplayer")
include(":app")
