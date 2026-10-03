#
# Qt6 QML 播放器组件（platform/QtPlayer）
#
# 这个工程有两个身份：
#   1. 组件本体：src/ 下的 CicadaPlayerItem（QML 元素）+ 零拷贝后端；
#   2. 示例程序：main.cpp + Main.qml，把组件跑起来。
#
# 构建方式和 cmdline 一样：**把框架当子目录一起编**（媒体框架、demuxer、解码器、
# 渲染器都在本仓库里），所以下面那一大段"框架部分"是从 cmdline/CMakeLists.txt
# 抄过来的，改框架的编译选项时要两处一起改。
#
cmake_minimum_required(VERSION 3.16)

# ===========================================================================
# vcpkg：Windows/MSVC 上框架要 pthreads(PThreads4W)、sdl2、libxml2，这些在 vcpkg 里
#
# 原则：**换机器不用改这个文件**。vcpkg 在哪、用哪个 triplet，全部取自当前机器 /
# 当前工具链的事实，文件里不出现任何具体路径：
#
#   * vcpkg 根目录：环境变量 VCPKG_ROOT、VCPKG_INSTALLATION_ROOT，其次是 PATH 里的
#     vcpkg（和 cmdline/msvc_build/build_win.bat 的 where vcpkg 一个路子）。
#     判定标准只有一条：该目录下确实存在 scripts/buildsystems/vcpkg.cmake。
#   * triplet：优先用调用方给的 VCPKG_TARGET_TRIPLET，其次 vcpkg 自己的环境变量
#     VCPKG_DEFAULT_TRIPLET，最后按本工程**实际生成的目标位数**推：
#     CMAKE_SIZEOF_VOID_P == 8 → x64-windows，== 4 → x86-windows，
#     ARM64 → arm64-windows。都是本次构建的事实，不是猜路径。
#
# 为什么是自己往 CMAKE_PREFIX_PATH 里加，而不是加载 vcpkg 工具链：
# CMAKE_TOOLCHAIN_FILE 只在**配置最开始**被读取（文档原文 "read early in the CMake
# run"），在 CMakeLists.txt 里 set 它对本次配置无效（CMakeCache.txt 里连这个条目都
# 不会出现），注入点 CMAKE_PROJECT_INCLUDE_BEFORE / CMAKE_PROJECT_TOP_LEVEL_INCLUDES
# 也都在它之后才执行。所以这里只做工具链里对"**找到包**"最管用的那一步：把
# installed/<triplet> 加进 CMAKE_PREFIX_PATH（vcpkg 的包 config 就在
# installed/<triplet>/share/<包名>/ 下，CMake 的 config 模式本来就会去搜）。
# 运行时 DLL 在文件末尾按同样的 triplet 拷到 exe 旁边，等价于工具链的 applocal。
#
# 想让 CMake 真正加载工具链（完全用 vcpkg 那一套）也可以：命令行 / kit 里传
#     -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
# 这种情况下下面整段自动跳过。
# ===========================================================================
project(QtPlayer VERSION 0.1 LANGUAGES CXX)

# ===========================================================================
# OBJCXX —— **和下面的静态/动态开关同一条规矩：必须在任何 add_subdirectory 之前**
#
# CMake 的"语言"是**目录作用域 + 向下继承**的：`enable_language(OBJCXX)` 只对**它之后**
# 进入的子目录生效。而 .mm 不只在本目录编：
#   * 本目录：src/CicadaTextureMetal.mm、src/CicadaQtTextureWrap.mm；
#   * 3rdparty/qwindowkit：src/core/cocoawindowcontext.mm、src/core/style/styleagent_mac.mm
#     —— 它是本文件下面 add_subdirectory(3rdparty/qwindowkit …) 进来的**子目录**。
#
# 原来这一句写在后面那段"平台源文件列表"里（在 add_subdirectory 之后），于是
# qwindowkit 的 .mm 所在作用域压根没有 OBJCXX，configure 能过、**生成阶段**才炸：
#     CMake Error: Error required internal CMake variable not set …
#     Missing variable is: CMAKE_OBJCXX_COMPILE_OBJECT      （一次十几条）
# 放到这里（project() 之后、第一个 add_subdirectory 之前）就对了。
#
# 只在 APPLE 上开：Windows/Linux 上没有 .mm，多开一门语言会拖慢配置。
# ===========================================================================
if (APPLE)
    enable_language(OBJCXX)
endif ()

# ===========================================================================
# 静态 / 动态 Qt —— **这个开关必须放在文件最前面，任何 add_subdirectory 之前**
#
# 用法（脚本 deploy_win.ps1 有对应的参数，见那个文件）：
#     动态：cmake -S . -B build\msvc        -DCMAKE_PREFIX_PATH=<动态 Qt 前缀>
#     静态：cmake -S . -B build\msvc-static -DCMAKE_PREFIX_PATH=<静态 Qt 前缀> -DCICADA_QT_STATIC=ON
#
# 【为什么位置这么靠前】静态 Qt 是用 **/MT（静态 MSVC CRT）** 编的
# （D:\Qt-Static\msvc\mkspecs\qconfig.pri 里 QT_CONFIG 带 static_runtime），
# 而本工程默认是 /MD。两者混链会得到：
#     LNK2038: "RuntimeLibrary" 不匹配 (MD_DynamicRelease vs MT_StaticRelease)
#     LNK2005: CRT 符号重复定义
# 而 CMAKE_MSVC_RUNTIME_LIBRARY 是**在创建 target 的那一刻被读取**的：框架那一堆静态库
# （mediaPlayer / demuxer / codec / render / utils / qwindowkit …）也必须一起走 /MT，
# 所以必须在下面第一个 add_subdirectory 之前设好 —— 放到后面设只对 appQtPlayer 生效，
# 链框架的库照样报 LNK2038。
#
# 【为什么是显式开关，不做自动探测】要判断"找到的 Qt 是不是静态版"必须等
# find_package(Qt6)，而那时候所有子目录已经加完了、target 已建、运行时库已经定死。
# 所以由调用方给这个开关（脚本会给），等到 find_package 之后再用 Qt 自己导出的
# QT6_IS_SHARED_LIBS_BUILD **校验一遍**：给错了当场 FATAL_ERROR 停下，
# 而不是编出一个链接不过、或者运行期行为诡异的产物。
#
# 【非 MSVC 平台】静态 Qt 在 Linux/macOS 上是常规做法，不需要额外开关
# （MSVC 运行库那套只对 MSVC 有意义），所以这里只在 MSVC 下设 /MT。
# ===========================================================================
option(CICADA_QT_STATIC "Link Qt statically (requires a static Qt prefix, e.g. D:/Qt-Static/msvc)" OFF)

if (CICADA_QT_STATIC AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif ()

# ---------------------------------------------------------------------------
# vcpkg 的 triplet：调用方给的 > vcpkg 自己的环境变量 > 本次构建的目标位数
# ---------------------------------------------------------------------------
if (NOT VCPKG_TARGET_TRIPLET)
    if (DEFINED ENV{VCPKG_DEFAULT_TRIPLET})
        set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "vcpkg triplet")
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64")
        set(VCPKG_TARGET_TRIPLET "arm64-windows" CACHE STRING "vcpkg triplet")
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")
    else ()
        set(VCPKG_TARGET_TRIPLET "x86-windows" CACHE STRING "vcpkg triplet")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# vcpkg 根目录：找**真的能服务本次构建的那个**
#
# 一台机器上常常有多个 vcpkg（例如 VS 2022 自带的 <VS>\VC\vcpkg 是空的，而包其实装在
# 另外一处），所以不能"取第一个找到的"。判定标准：该根目录下确实存在
# installed/<triplet>（框架要的 pthreads / sdl2 / libxml2 就装在那儿）。
#
# 候选来源（与 cmdline/msvc_build/build_win.bat 的找法一致，文件里没有任何具体路径）：
#   * 环境变量 VCPKG_ROOT、VCPKG_INSTALLATION_ROOT
#   * PATH 里的 vcpkg（脚本里的 where vcpkg）
#
# 为什么不直接加载 vcpkg 工具链：CMAKE_TOOLCHAIN_FILE 只在配置最开始被读取（文档
# "read early in the CMake run"），在 CMakeLists 里 set 它本次配置无效。所以这里做
# 工具链里对"找到包"最管用的两步：设好 VCPKG_INSTALLED_DIR / _VCPKG_INSTALLED_DIR
# （vcpkg 生成的包 config 就靠这两个变量找自己的头文件和库，而且带 NO_DEFAULT_PATH），
# 再把 installed/<triplet> 加进 CMAKE_PREFIX_PATH。运行时 DLL 在文件末尾按同样的
# triplet 拷到 exe 旁边，等价于工具链的 applocal。
#
# 调用方已经给了 -DCMAKE_TOOLCHAIN_FILE 时，下面整段跳过（完全交给 vcpkg 那一套）。
# ---------------------------------------------------------------------------
if (NOT CMAKE_TOOLCHAIN_FILE)
    set(_cicada_candidates "")

    foreach (_cicada_env_root IN ITEMS "$ENV{VCPKG_ROOT}" "$ENV{VCPKG_INSTALLATION_ROOT}")
        if (_cicada_env_root AND EXISTS "${_cicada_env_root}/scripts/buildsystems/vcpkg.cmake")
            list(APPEND _cicada_candidates "${_cicada_env_root}")
        endif ()
    endforeach ()

    file(TO_CMAKE_PATH "$ENV{PATH}" _cicada_path)

    foreach (_cicada_dir IN LISTS _cicada_path)
        if (EXISTS "${_cicada_dir}/scripts/buildsystems/vcpkg.cmake")
            list(APPEND _cicada_candidates "${_cicada_dir}")
        endif ()
    endforeach ()

    unset(_cicada_path)
    list(REMOVE_DUPLICATES _cicada_candidates)

    set(CICADA_VCPKG_ROOT "")

    foreach (_cicada_candidate IN LISTS _cicada_candidates)
        if (EXISTS "${_cicada_candidate}/installed/${VCPKG_TARGET_TRIPLET}")
            set(CICADA_VCPKG_ROOT "${_cicada_candidate}")
            break()
        endif ()
    endforeach ()

    if (NOT CICADA_VCPKG_ROOT AND _cicada_candidates)
        list(GET _cicada_candidates 0 _cicada_first)

        message(WARNING
                "QtPlayer: found vcpkg at ${_cicada_first}, but it has no "
                "installed/${VCPKG_TARGET_TRIPLET} (the framework needs pthreads, sdl2 and "
                "libxml2 from vcpkg). Install them there:\n"
                "    \"${_cicada_first}/vcpkg.exe\" install pthreads sdl2 libxml2 "
                "--triplet ${VCPKG_TARGET_TRIPLET}\n"
                "or set VCPKG_ROOT to the vcpkg that already has them "
                "(candidates checked: ${_cicada_candidates})")

        # 仍然指过去，好让框架的报错里带上一个具体路径
        set(CICADA_VCPKG_ROOT "${_cicada_first}")
    endif ()

    unset(_cicada_candidates)
    unset(_cicada_first)
    set(CICADA_VCPKG_ROOT "${CICADA_VCPKG_ROOT}" CACHE INTERNAL "vcpkg root (empty = not found)")
endif ()

# ---------------------------------------------------------------------------
# vcpkg 根目录统一转成正斜杠。
#
# 路径是从环境变量（%VCPKG_ROOT%）来的，Windows 上写的是反斜杠：D:\Dev\vcpkg。
# 直接把它拼进 install(FILES ...) 会出事：CMake 会把这条规则原样写进生成的
# build/<dir>/cmake_install.cmake，而那里的字符串要按 CMake 语法再解析一遍，
# "\D" 是非法转义，安装阶段直接报：
#     Syntax error in cmake code at .../cmake_install.cmake:74
#     when parsing string D:\Dev\vcpkg/installed/x64-windows/bin/pthreadVC3.dll
#     Invalid character escape '\D'.
# CMake 内部一律用正斜杠（file(TO_CMAKE_PATH) 就是干这个的），转完两边都干净。
#
# 放在发现块外面、无条件执行：显式 -DCICADA_VCPKG_ROOT=... 或走工具链时也可能带反斜杠。
# 这是普通变量，会遮住同名的 cache 项；每次配置都会重跑这段，所以每次都干净。
# ---------------------------------------------------------------------------
if (CICADA_VCPKG_ROOT)
    file(TO_CMAKE_PATH "${CICADA_VCPKG_ROOT}" CICADA_VCPKG_ROOT)
endif ()

if (CICADA_VCPKG_ROOT)
    set(VCPKG_INSTALLED_DIR "${CICADA_VCPKG_ROOT}/installed" CACHE PATH "vcpkg installed dir")
    set(_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}")

    list(APPEND CMAKE_PREFIX_PATH
            "${CICADA_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}"
            "${CICADA_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/debug")

    message(STATUS "QtPlayer: vcpkg ${CICADA_VCPKG_ROOT} (triplet ${VCPKG_TARGET_TRIPLET}) "
                   "added to CMAKE_PREFIX_PATH")
endif ()

set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 源码是 UTF-8（无 BOM），MSVC 默认按 ANSI(cp936) 解码会把中文注释和紧随其后的
# 代码行搅在一起（详见 cmdline/CMakeLists.txt 里的说明）。这里同样显式声明。
# ---------------------------------------------------------------------------
if (MSVC)
    add_compile_options(/utf-8)
endif ()

# ---------------------------------------------------------------------------
# 非 MSVC 平台预包含几个 C 标准头：这棵树里大量文件依赖"MSVC 会顺带把 <cstring>
# 之类拉进来"这一行为（同样见 cmdline/CMakeLists.txt）。
# ---------------------------------------------------------------------------
if (NOT MSVC)
    foreach (_cicada_std_header cstdint cstring cstdio cstdlib)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -include ${_cicada_std_header}")
    endforeach ()
    foreach (_cicada_std_header stdint.h string.h stdio.h stdlib.h)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -include ${_cicada_std_header}")
    endforeach ()
endif ()

set(CMAKE_CXX_STANDARD 11)

# ===========================================================================
# 框架部分（和 cmdline/CMakeLists.txt 保持一致）
#
# 注意路径深度：本工程在 CicadaPlayerNext/platform/QtPlayer，比 cmdline 深一层，
# 所以引用仓库里的框架要用 ../../（cmdline 里是 ../）。全部走下面这个绝对路径变量，
# 免得再数错层级 —— CMake 的相对路径是按"当前 CMakeLists.txt 所在目录"算的。
# ===========================================================================
set(CICADA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(CICADA_ROOT ${CICADA_ROOT} ABSOLUTE)

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(TARGET_PLATFORM macOSX)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(TARGET_PLATFORM Linux)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    set(TARGET_PLATFORM windows)
endif ()

include(${CICADA_ROOT}/framework/code_coverage.cmake)
add_subdirectory(${CICADA_ROOT}/mediaPlayer mediaPlayer.out)
add_subdirectory(${CICADA_ROOT}/plugin plugin.out)

include(${CICADA_ROOT}/framework/${TARGET_PLATFORM}.cmake)
include(${CICADA_ROOT}/framework/module_config.cmake)

# ===========================================================================
# Qt6
# ===========================================================================
find_package(Qt6 6.5 REQUIRED COMPONENTS Quick WebSockets)

# ===========================================================================
# 网页窗口（WebWindow.qml）：首页「HTML 链接 / 打开网页」那个窗口
#
# 【依赖】QtWebView + **WebView2 SDK**。注意 WebView2 有两个东西，别混：
#   * **运行时**（Edge WebView2 Runtime）—— Win10/11 自带，跑起来才需要；
#   * **SDK**（WebView2.h + WebView2LoaderStatic.lib）—— **编译**时才需要，
#     静态 Qt 的 Qt6WebView 配置会去 find_package(WebView2) 找它：
#       D:/Qt-Static/msvc/lib/cmake/Qt6WebView/Qt6QWebView2WebViewPluginDependencies.cmake
#           set(__qt_QWebView2WebViewPlugin_third_party_deps "WebView2;FALSE;;;")
#
# 【为什么不用 REQUIRED、而是探测】这台机器上原本没装 SDK（CMakeCache 里留着
# WEBVIEW2_INCLUDE_DIR-NOTFOUND / WEBVIEW2_LIBRARY-NOTFOUND）。写成 REQUIRED 的话，
# 没装 SDK 的机器会在**配置阶段**直接失败，把整个工程带崩 —— 一个附加功能不该有这个权力。
# 所以：QUIET 找一次，用 TARGET Qt6::QWebView2WebViewPlugin 判断后端插件到底有没有；
# 有就打开（把 WebWindow.qml 放进 qrc），没有就自动降级。
#
# 装 SDK（一次即可，~10 MB，不用装到系统里）：
#     nuget 或 https://www.nuget.org/packages/Microsoft.Web.WebView2 下 .nupkg（就是 zip）
#     解到 D:\webview2sdk        （解完要能看到 build\native\include\WebView2.h）
#     setx WEBVIEW2_SDK_ROOT D:\webview2sdk      （新开终端生效）
# 没装/没降级时功能不是"没了"：首页那颗按钮仍然可用，会退回**系统浏览器**打开
#（见 HomeWindow.openWebUrl），不做"点了没反应"。
# ===========================================================================
option(CICADA_ENABLE_WEBVIEW "构建首页的网页窗口（需要 QtWebView + WebView2 SDK）" ON)

set(CICADA_WEB_WINDOW_QML "")
if (CICADA_ENABLE_WEBVIEW)
    find_package(Qt6 6.5 QUIET COMPONENTS WebView)

    if (TARGET Qt6::QWebView2WebViewPlugin)
        # 这一行同时干两件事：把 WebWindow.qml 放进 qrc（否则 createComponent 找不到文件），
        # 以及让 qt_import_qml_plugins 扫到 `import QtWebView`（静态构建靠它自动链 QML 插件）。
        set(CICADA_WEB_WINDOW_QML "WebWindow.qml")
        message(STATUS "QtPlayer: web window ENABLED (QtWebView + WebView2 SDK found)")
    else ()
        message(STATUS "QtPlayer: web window DISABLED (WebView2 SDK not found; "
                       "首页「打开网页」会退回系统浏览器。装法见 CMakeLists 里那段说明："
                       "解包 Microsoft.Web.WebView2 到 D:\\webview2sdk 并设 WEBVIEW2_SDK_ROOT)")
    endif ()
else ()
    message(STATUS "QtPlayer: web window DISABLED by -DCICADA_ENABLE_WEBVIEW=OFF")
endif ()
qt_standard_project_setup()

# ---------------------------------------------------------------------------
# 静态/动态开关的**事后校验**：和实际找到的 Qt 对不上就当场停下
#
# Qt 自己会导出 QT6_IS_SHARED_LIBS_BUILD（Qt6CoreConfigExtras.cmake 里 set 的字符串
# "ON"/"OFF"；静态 Qt 那份是 "OFF"）。它的值和我们的开关必须一致 ——
# 不一致的后果不是"编不过"，而是：
#   * 开关说静态、Qt 其实是动态 → 少了 /MT 和静态插件导入，链接/运行期一堆怪问题；
#   * 开关说动态、Qt 其实是静态 → 全都静默地按 /MD 编，最后 LNK2038。
# 两种都很难从报错反推原因，所以在这里直接拦掉。
# （老 Qt 没导出这个变量时只打一行提示，不做判断。）
# ---------------------------------------------------------------------------
if (DEFINED QT6_IS_SHARED_LIBS_BUILD)
    if (CICADA_QT_STATIC AND QT6_IS_SHARED_LIBS_BUILD)
        message(FATAL_ERROR
                "QtPlayer: CICADA_QT_STATIC=ON, but the Qt at ${Qt6_DIR} is a SHARED build.\n"
                "Point CMAKE_PREFIX_PATH at the static Qt instead, e.g.\n"
                "    -DCMAKE_PREFIX_PATH=D:/Qt-Static/msvc -DCICADA_QT_STATIC=ON")
    endif ()

    if (NOT CICADA_QT_STATIC AND NOT QT6_IS_SHARED_LIBS_BUILD)
        message(FATAL_ERROR
                "QtPlayer: the Qt at ${Qt6_DIR} is a STATIC build, but CICADA_QT_STATIC is OFF.\n"
                "A static Qt needs the /MT runtime and the static plugin imports, so pass\n"
                "    -DCICADA_QT_STATIC=ON\n"
                "(deploy_win.ps1 -Static does it for you).")
    endif ()

    if (CICADA_QT_STATIC)
        message(STATUS "QtPlayer: linking a STATIC Qt (MSVC CRT = /MT, QML plugins imported)")
    else ()
        message(STATUS "QtPlayer: linking a SHARED Qt (Qt DLLs are deployed by the install step)")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# Qt Svg：**只有静态 Qt 需要在这里显式找**
#
# 动态 Qt 下 `Image { source: "xxx.svg" }` 走的是运行期插件 plugins/imageformats/qsvg.dll
# （部署脚本会拷）；静态 Qt 下没有运行期插件加载这回事，必须把 Qt6::QSvgPlugin 链进去，
# 否则所有 SVG 素材（next-play.svg、assets/images/dm/*.svg…）都画不出来。
# 用 QUIET + TARGET 判断：没装 Qt Svg 的开发机照样能编（只是没有 SVG 图标）。
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 QUIET COMPONENTS Svg)

# ---------------------------------------------------------------------------
# QTP0001：QML 模块的资源前缀 —— **必须设成 NEW**，否则部署出去的程序起不来
#
# 这是"构建/部署之后运行报 Module "QtPlayer" contains no type named "Main"" 的根因。
#
# Qt 自己的 Qt6QmlMacros.cmake（6.11.1 第 811~826 行）是这么决定前缀的：
#     qt6_policy(GET QTP0001 use_auto_prefix_policy)
#     if ("${use_auto_prefix_policy}" STREQUAL "NEW")
#         set(arg_RESOURCE_PREFIX "/qt/qml")
#     else()
#         set(arg_RESOURCE_PREFIX "/")          # ← 策略没设时走这里
#     endif()
# 前缀为 "/" 时，模块被嵌到 **:/QtPlayer/**（可以看构建目录里的
# .qt/rcc/appQtPlayer_raw_qml_0.qrc，里面就是 prefix="/QtPlayer/"），而 QML 引擎的默认
# 导入路径里只有 :/qt/qml 和 :/qt-project.org/imports —— 于是运行期找不到这个模块的
# qmldir，只剩 C++ 侧 qmlRegisterModule 注册出来的"空模块"：
# `import QtPlayer; Main {}` 里 Main 是 qmldir 里声明的 QML 文件类型，自然就"没有这个类型"。
#
# 为什么在 Qt Creator 里跑没事：Qt Creator 会把**构建目录**放进 QML_IMPORT_PATH，
# 而构建目录里有 Qt 生成的那份 QtPlayer/qmldir + Main.qml，模块从文件系统被找到了。
# 一旦把 exe 拷到别处（部署），这个目录不存在了 —— 报错就是这个。
#
# 设成 NEW 之后模块嵌到 :/qt/qml/QtPlayer/（默认导入路径里），exe 自带完整模块，
# 不需要任何额外文件、也不依赖构建目录。
# ---------------------------------------------------------------------------
if (QT_KNOWN_POLICY_QTP0001)
    qt_policy(SET QTP0001 NEW)
else ()
    message(WARNING "QtPlayer: this Qt version has no QTP0001 policy; the QML module may "
                    "not be embedded in the default import path and the deployed executable "
                    "could fail with 'Module \"QtPlayer\" contains no type named \"Main\"'")
endif ()

set(CMAKE_AUTOMOC ON)

# ===========================================================================
# QWindowKit：跨平台无边框窗口
#
# 来源：HuskarUI 的 3rdparty（vendored 在本目录 3rdparty/qwindowkit，含它依赖的
# qmsetup 辅助 CMake 包，自包含、不联网）。HuskarUI 自己就是这么配的
# （见它的 CMakeLists.txt），下面四个 option 和它保持一致。
#
# 为什么用它而不是自己写原生代码：
#   * Windows 上走原生 WM_NCHITTEST：贴边布局（Aero Snap）、双击标题栏最大化、
#     右键系统菜单、多屏不同 DPI 的命中测试，全部是系统行为，自己模拟很难做对；
#   * macOS / Linux 各自有原生实现（QWindowKit 内部按平台分文件）；
#   * 它**只管窗口系统这一层**，完全不碰图形 API —— 场景图用哪个 RHI 后端、用哪块
#     GPU 设备，仍然百分之百由 Qt 决定，我们不插入任何东西。
#
# 编译成**静态库**：不产生额外 DLL，部署脚本不用动（qmsetup 只做 CMake 辅助，也不进运行时）。
# 只开 QUICK（QML 的 WindowAgent）—— WIDGETS 是 QtWidgets 版，我们用不上。
#
# Windows 上的"系统边框"开关（QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS）决定窗口四周
# 那圈边框由谁画 —— 两种模式 QWindowKit 都实现了（见它 README 的 Qt Quick 一节）：
#
#   * ON（它的默认值，也是 HuskarUI 现在用的）：保留窗口左/右/下三边的**原生边框**，
#     只去掉顶部那块非客户区。代价是：
#       - 窗口四周有一圈系统画的边框（深色/强调色下就是"窗口外面一圈黑边"）；
#       - Windows 10 上"顶部那条边框"就没人画了，于是 QWindowKit 自己建一个
#         QQuickPaintedItem 在窗口最上面画一条 1px 的线
#         （core/shared/windows10borderhandler_p.h，颜色是它内置的
#         activeLight/activeDark = #6E6E6E/#333333）—— 这就是"顶部那条黑线"。
#         而且在 Win10 上**应用侧关不掉**：属性 "win10-border-needed" 是只读的
#         （`return isSystemBorderEnabled() && !isWin11OrGreater();`）。
#
#   * OFF（QWindowKit README 里说的 "borderless version"）：WM_NCCALCSIZE 直接把客户区
#     铺满整个窗口 —— 四周没有系统边框，也不会建那条自绘顶线；四周的缩放改由它自己在
#     WM_NCHITTEST 里算（上/下/左/右 + 四个角都实现了，见 win32windowcontext.cpp 里
#     isSystemBorderEnabled() 为假的那个分支），窗口阴影靠 DWM 的 1px extra-margins 保留
#     （注释里写明：没有这 1px，DWM 就不画阴影了）。
#
# 我们要的是"没有外边框 + 内容铺到窗口边缘"的沉浸式外观，所以选 OFF。
# 反过来，如果你更想要贴着系统原生边框的样子，把它改回 ON 就行（顶线也会跟着回来）。
set(QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS OFF)

set(QWINDOWKIT_BUILD_STATIC ON)
set(QWINDOWKIT_BUILD_WIDGETS OFF)
set(QWINDOWKIT_BUILD_QUICK ON)
set(QWINDOWKIT_BUILD_EXAMPLES OFF)
set(QWINDOWKIT_BUILD_DOCUMENTATIONS OFF)
set(QWINDOWKIT_INSTALL OFF)

add_subdirectory(3rdparty/qwindowkit qwindowkit.out)

# ===========================================================================
# 弹幕引擎（独立模块，CicadaPlayerNext/danmaku）
#
# 它是**纯 C++11 + 标准库**写的，不依赖 Qt、也不依赖本框架 —— 故意这样：
# 那一份可以单独编成 Danmaku.dll 给别的项目用（它自己有 305 项自检，
# 见 danmaku/README.md）。这里只是把它作为子工程拉进来，然后在 appQtPlayer 里链接。
#
# 第二个参数 danmaku.out 是二进制目录（和上面 qwindowkit.out 一个约定），
# 免得它的中间产物混进本工程的构建目录。
#
# 注意：它自带 install 规则只在"独立构建"时生效（见 danmaku/CMakeLists.txt 末尾），
# 作为子工程不会往本工程的 install 里塞东西。
# ===========================================================================
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../danmaku danmaku.out)

# ===========================================================================
# 组件源码
# ===========================================================================
set(CICADA_QT_SOURCES
        src/CicadaPlayerItem.cpp
        src/CicadaPlayerItem.h
        src/CicadaVideoRender.cpp
        src/CicadaVideoRender.h
        src/CicadaVideoTexture.cpp
        src/CicadaVideoTexture.h
        src/CicadaHardwareDevice.cpp
        src/CicadaHardwareDevice.h
        # 原生纹理 -> QSGTexture 的包装层（Qt 6 删掉了 createTextureFromNativeObject，
        # 这里封装 6.8+ 的 native interface 和 6.5~6.7 的 QRhi 兜底两条路）
        src/CicadaQtTextureWrap.cpp
        src/CicadaQtTextureWrap.h
        # 弹幕：把独立引擎（Danmaku）接到 QML 的薄适配层。
        # 算法一行都不在这里 —— 全在 ../../danmaku 那个模块里，这里只做属性/列表转换。
        src/DanmakuController.cpp
        src/DanmakuController.h
        # 弹幕 GPU 批量渲染（换掉了原来 QML 对象池那条路）：
        #   DanmakuAtlas        —— 把每条弹幕烤进一张纹理图集（文字/描边/白框都在里面）
        #   DanmakuRendererItem —— QQuickItem：每条在屏弹幕一个 QSGImageNode，
        #                          只画图集里属于它的那块子矩形（Qt 自带材质，
        #                          **不用自定义 QSGMaterial / .qsb**，原因见文件头注释）
        # 详见各文件头注释。
        src/DanmakuAtlas.cpp
        src/DanmakuAtlas.h
        src/DanmakuRendererItem.cpp
        src/DanmakuRendererItem.h
        # 本地视频库：递归扫描文件夹 + 抽首帧缩略图缓存 + 当播放列表（上一集/下一集/选集共用）。
        src/VideoLibrary.cpp
        src/VideoLibrary.h
        # 进度条悬停预览的"按位置独立抽帧"（专属工作线程 + 有界 LRU 缓存，见那个文件头）：
        # 抽帧本体复用 VideoLibrary 里那段已经在跑的 grabFrameAt，这里只负责调度与缓存。
        src/PreviewFrameSource.cpp
        src/PreviewFrameSource.h
        # 应用级文件/目录选择对话框（**从 C++ 调**，见那个文件头：QML 的 FileDialog 会占住
        # 原生句柄不释放，这里每次现建现销）。
        src/AppFileDialogs.cpp
        src/AppFileDialogs.h
        )

# 每个平台一套零拷贝后端：Windows 走 D3D11 视频处理器，macOS 走 Metal，
# Linux 走 VAAPI/dmabuf/EGL。没有对应后端时 CicadaVideoTexture 会自动退到
# CPU 路径（下载 + 上传），所以三端都能编、都能播。
if (WIN32)
    list(APPEND CICADA_QT_SOURCES
            src/CicadaTextureD3D11.cpp
            src/CicadaTextureD3D11.h)
elseif (APPLE)
    enable_language(OBJCXX)
    list(APPEND CICADA_QT_SOURCES
            src/CicadaTextureMetal.mm
            src/CicadaTextureMetal.h
            # Metal 那一份包装要写 Objective-C（id<MTLTexture>），所以是 .mm
            src/CicadaQtTextureWrap.mm)
else()
    list(APPEND CICADA_QT_SOURCES
            src/CicadaTextureVAAPI.cpp
            src/CicadaTextureVAAPI.h)
endif ()

# ---------------------------------------------------------------------------
# 关掉 QML 调试器
#
# 打开时 Qt 会在进程启动时打印：
#   "QML debugging is enabled. Only use this in a safe environment."
# 这条不是错误，只是提示"QML 调试端口开着"（Qt Creator 的 Debug 配置默认把它打开：
# build/...-Debug/CMakeCache.txt 里 QT_ENABLE_QML_DEBUG=ON）。
#
# 这里用**普通变量**把它盖成 OFF（普通变量优先于同名缓存变量，不用去改别人的缓存）：
# 之后 Qt 的宏就不会再往编译定义里加 QT_QML_DEBUG，那条提示自然消失。
# 需要调 QML 的时候把它临时改回 ON 即可。
# ---------------------------------------------------------------------------
set(QT_ENABLE_QML_DEBUG OFF)

qt_add_executable(appQtPlayer
        main.cpp
        )

# ---------------------------------------------------------------------------
# Windows 应用程序清单（Common-Controls v6 + PerMonitorV2）
#
# 少了它，进程拿到的是"经典"视觉样式：系统原生的"打开文件"对话框（#32770）会画成
# Win95 那种方框按钮，看起来像"Qt 自己画的选择器"，其实一直是系统原生的那个。
# 这一条**只能写在清单里**（激活上下文没法用代码设置）。
#
# 把 .manifest 加进 target_sources 时，CMake/MSVC 会把它作为 /MANIFESTINPUT 交给链接器，
# 与链接器自己生成的 UAC 段合并，不需要额外开关。非 Windows 平台直接不加。
# ---------------------------------------------------------------------------
if (WIN32 AND MSVC)
    target_sources(appQtPlayer PRIVATE appQtPlayer.manifest)
endif ()

# ---------------------------------------------------------------------------
# 应用图标（Windows 的 .ico / macOS 的 .icns）
#
# 【Windows】必须把 .rc 交给链接器：exe 的图标是 PE 里的 RT_GROUP_ICON 资源，
# 不是运行时画出来的。少了它，资源管理器 / 任务栏 / Alt-Tab 上永远是系统默认图标
# （Qt 的 setWindowIcon() 只影响窗口标题栏，替不了这个）。
# 图标本体是多尺寸 ICO，由 tools/make_icons/make_icons.py 从 doc/Cicada.svg 生成。
#
# 【macOS】MACOSX_BUNDLE_ICON_FILE 只写**文件名**，CMake 会据此在
# appQtPlayer.app/Contents/Resources/<名字> 找它，并且只有放在 bundle 里才生效，
# 所以下面用 MACOSX_PACKAGE_LOCATION Resources 把 .icns 归到 bundle 内那一层。
# 同一个属性还会把 CFBundleIconFile 写进 Info.plist —— 不需要手改 plist。
#
# 【顺序】这两个 if 都必须**在 qt_add_executable 之后**：target_sources() 要求目标
# 已存在。macOS 的 set_target_properties 与下面那段 MACOSX_BUNDLE_BUNDLE_VERSION
# 合并生效（CMake 的 set_target_properties 是按属性覆盖，不是整体替换）。
# ---------------------------------------------------------------------------
if (WIN32 AND MSVC)
    target_sources(appQtPlayer PRIVATE appQtPlayer.rc)
endif ()

if (APPLE)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/appicon/Cicada.icns")
        target_sources(appQtPlayer PRIVATE assets/appicon/Cicada.icns)
        set_source_files_properties(assets/appicon/Cicada.icns PROPERTIES
                MACOSX_PACKAGE_LOCATION Resources)
        set_target_properties(appQtPlayer PROPERTIES MACOSX_BUNDLE_ICON_FILE Cicada.icns)
    else ()
        # 不静默：.icns 是生成物（tools/make_icons/make_icons.py），丢了就明确说一句，
        # 免得到时候"构建成功但双击 .app 是个默认图标"而没人知道为什么。
        message(WARNING "QtPlayer: assets/appicon/Cicada.icns 不存在，macOS 应用图标不会被设置。"
                " 请先运行 `python tools/make_icons/make_icons.py`（或在 macOS 上跑 "
                "assets/appicon/make_icns.sh 用 iconutil 生成）。")
    endif ()
endif ()

# QML 模块：URI "QtPlayer" 对应 QML 里的 `import QtPlayer`；
# QML_ELEMENT 注册的类型由 qt_add_qml_module 自动生成注册代码。
#
# 注意：组件源码只在这里列一次 —— qt_add_qml_module 会把 SOURCES 里的文件加到
# 目标上，同时交给 moc 和 qqmltyperegistrar 处理。如果再在 qt_add_executable 里
# 列一遍，同一个 .cpp 会被加进目标两次。
#
# QtPlayerTheme.qml 是**单例**（那个文件里写了 `pragma Singleton`）：必须在这里用
# QT_QML_SINGLETON_TYPE 标一下，生成的 qmldir 里才会带 `singleton` 一行。
# 少了这一句，QML 会把 QtPlayerTheme 当普通组件，用的时候报 "QtPlayerTheme is not a type"。
set_source_files_properties(QtPlayerTheme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# ---------------------------------------------------------------------------
# 界面素材（**原样**拷自参考工程 front/hili-player/packages/player/src/assets）：
#   * assets/lottie-icon/*.json —— 控制栏那 20 个 Lottie 动画图标；
#   * assets/images/*          —— 参考里用到的图片素材。其中 control-mask.png 是从
#                                 controls.scss:9 里那段 base64 解出来的同一张图
#                                 （1x198 的 PNG，控制条下方那层"透明 -> 黑"的遮罩，
#                                 参考里是 background: url(...) repeat-x bottom）；
#                                 其余 state.svg / play.svg / pause.svg / buffer.webp /
#                                 ploading.png 是播放状态图标、缓冲与加载图。
#
# 用 file(GLOB) 而不是一条条列：这些是"数据文件"，不是源码 —— 加新素材时不用改 CMake。
# 代价是**新增/删除素材后要重新 configure** 一次（CMake 不会因为目录变化自动重跑）。
#
# 走 RESOURCES：它们会被打进 QML 模块资源里（QTP0001 策略下模块在 /qt/qml/QtPlayer/），
# 所以 QML 里用相对路径 "assets/lottie-icon/xxx.json" / "assets/images/xxx" 就能取到，
# 而且不用单独部署文件。
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 【CONFIGURE_DEPENDS 是后加的，别删】上面那句"新增素材要重新 configure 一次"就是这个
# file(GLOB) 的毛病：glob 只在 configure 时展开，新拷进来的素材（例如高能进度条那颗 pin 的
# assets/images/pbp-pin-off.svg、pbp-pin-on.svg）不会自己进 RESOURCES，
# 表现是"程序里那张图是空的、日志里一条 Image 加载失败"。
# 加上 CONFIGURE_DEPENDS 之后，构建时会顺带检查这几个目录，变了就自动重新 configure，
# 不用再靠人记得去跑一次 cmake -S/-B。（本次改动仍需**重新 configure 一次**才生效。）
# ---------------------------------------------------------------------------
file(GLOB CICADA_LOTTIE_ICONS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/lottie-icon/*.json)
# 弹幕设置面板用的 SVG（参考 player/src/component/dmsetting 的四个类型图标 +
# icons 目录里的弹幕开关/齿轮）单独放在 assets/images/dm/ 一层，glob 不会递归，要显式列出。
#
# 【必须写 LIST_DIRECTORIES false】file(GLOB) 默认**连目录一起匹配**：
# assets/images/dm 这个子目录本身会被 assets/images/* 收进去，而 RESOURCES 里拿到目录时
# 会在 qt_add_qml_module 的拷贝步骤炸掉（_qt_internal_qml_copy_file → file ... 失败）。
file(GLOB CICADA_QT_IMAGE_ASSETS
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/images/*
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/images/dm/*)
list(SORT CICADA_LOTTIE_ICONS)
list(SORT CICADA_QT_IMAGE_ASSETS)
list(LENGTH CICADA_LOTTIE_ICONS _cicada_icon_count)
list(LENGTH CICADA_QT_IMAGE_ASSETS _cicada_image_count)
message(STATUS "QtPlayer: Lottie 图标 ${_cicada_icon_count} 个（assets/lottie-icon/*.json）、"
               "界面素材 ${_cicada_image_count} 个（assets/images/*）")

qt_add_qml_module(appQtPlayer
        URI QtPlayer
        VERSION 1.0
        QML_FILES
        Main.qml
        QtPlayerTheme.qml
        LottieIcon.qml
        PlayerView.qml
        PlayerControlBar.qml
        # 右键菜单 + 它打开的三个面板 + 提示条（全部复刻参考实现的 DOM/CSS）：
        #   PlayerContextMenu —— .player-contextmenu
        #   PlayerInfoPanel   —— .player-info-container（统计信息，数据来自 player.stats）
        #   PlayerColorPanel  —— .player-color-panel（色彩调整）
        #   PlayerHotkeyPanel —— .player-hotkey-panel（快捷键说明）
        #   PlayerToast       —— .player-toast-wrap
        PlayerContextMenu.qml
        PlayerInfoPanel.qml
        PlayerColorPanel.qml
        PlayerHotkeyPanel.qml
        PlayerToast.qml
        controls/ProgressRow.qml
        controls/LeftControls.qml
        controls/DanmakuBar.qml
        controls/RightControls.qml
        PlayerTopBar.qml
        DanmakuView.qml
        DanmakuItem.qml
        # 实时弹幕的 WebSocket 那一小块（mock 的 /danmaku/ws）。
        # 【必须列在这里】qt_add_qml_module 只把 QML_FILES 里的文件放进 qrc ——
        # 漏了这一行，DanmakuView.qml 里那个 Loader 会在运行时报
        #     qrc:/qt/qml/QtPlayer/DanmakuLive.qml: No such file or directory
        # 实时弹幕被静默降级成"不可用"（日志里那一行就是这么来的）。
        # 静态构建也靠它：qt_import_qml_plugins 是扫 QML_FILES 里的 import 链 QML 插件的，
        # 文件不在清单里 → `import QtWebSockets` 扫不到 → 静态包里也缺这个模块。
        DanmakuLive.qml
        DanmakuSettingPanel.qml
        SubtitlePanel.qml
        SettingsPanel.qml
        # 应用设置页（遮罩 + 面板 + 两个手写控件）。**Main.qml 和 HomeWindow.qml 共用同一份**
        # —— 从 Main.qml 里抽出来的，见那个文件的头注释和 HANDOVER-TODO.md 第三节第 1 条。
        SettingsPage.qml
        HomeWindow.qml

        # ---------------------------------------------------------------------------
        # 网页窗口（首页那个"HTML 链接"输入框打开的独立窗口）：
        #   WebWindow.qml —— Window + QWindowKit（和 HomeWindow 同一条路）+ 顶部标题栏
        #   （**逐行复制 HomeWindow 那套 CaptionButton / WindowAgent 登记**）+ 中间一个 WebView。
        #
        # 【为什么是变量而不是直接写文件名】这个窗口依赖 QtWebView + WebView2 SDK，
        # 默认关（见上面 CICADA_ENABLE_WEBVIEW 那一大段说明）。打开时才把文件放进 qrc：
        #   * qt_add_qml_module 只把 QML_FILES 里的文件放进 qrc，不在里面 → 不在 qrc 里
        #     → HomeWindow 用 createComponent 建它会失败（那边会退回系统浏览器）；
        #   * 这一行还负责让 qt_import_qml_plugins 扫到 `import QtWebView`
        #     （静态构建下 QML 插件靠扫 QML_FILES 的 import 链自动链上）。
        # ---------------------------------------------------------------------------
        ${CICADA_WEB_WINDOW_QML}

        # ---------------------------------------------------------------------------
        # 直播播放器（**一套独立的 UI**，用户要求不要在点播那套上改）：
        #   LiveMain       —— 直播窗口（无边框 + QWindowKit + 顶部热区浮出标题栏）
        #   LivePlayerView —— 直播主界面（画面 + 弹幕层 + 顶部房间信息 + 控制条 + 清晰度换流 + 自动重连）
        #   controls/LiveControlBar  —— 直播控制条（没有进度条！多了"直播中"状态与弹幕输入）
        #   controls/LiveIconButton  —— 控制条上的一颗图标按钮
        #   controls/LiveMenu        —— 直播里的浮层菜单（当前是清晰度档位）
        #
        # 【必须列在这里】qt_add_qml_module 只把 QML_FILES 里的文件放进 qrc ——
        # 漏一行就是运行时 "No such file or directory"（HomeWindow 那边已经踩过一次，
        # 见上面 DanmakuLive.qml 的注释）：HomeWindow 用
        # Qt.createComponent("qrc:/qt/qml/QtPlayer/LiveMain.qml") 建直播窗口，
        # 文件不在 qrc 里的话，点「直播 FLV 地址」只会得到一句"直播窗口创建失败"。
        # ---------------------------------------------------------------------------
        LiveMain.qml
        LivePlayerView.qml
        controls/LiveControlBar.qml
        controls/LiveIconButton.qml
        controls/LiveMenu.qml

        RESOURCES
        ${CICADA_LOTTIE_ICONS}
        ${CICADA_QT_IMAGE_ASSETS}
        SOURCES
        ${CICADA_QT_SOURCES}
        )

# ===========================================================================
# 【必须】给 QML 类型注册生成物补上头文件依赖 —— 这是"关窗即崩"的根因，别再删
#
# `appqtplayer_qmltyperegistrations.cpp`（qt_add_qml_module 生成的）里展开的是**每个
# QML_ELEMENT 类的 sizeof**（`qmlRegisterTypesAndRevisions<cicadaqt::DanmakuRendererItem>`），
# 而 QML 引擎正是按这个 sizeof 分配对象内存的（qqmlprivate.h 的 QQmlElement::createInto）。
# 但在这个 build 树里，那个 obj 的依赖表**只有它自己**（见
# CMakeFiles/appQtPlayer.dir/compiler_depend.internal）：它只在 moc 的 json 变化时才重新生成，
# 而**成员布局变化不会改变 json** ⇒ 改了 C++ 成员（哪怕只是删掉一个成员）它也不会重编
# → QML 按**旧 sizeof** 分配对象 → 构造/析构越界写堆。
#
# 实测现象（用户日志 + 子代理逐行核对）：关播放器窗口时崩在
#     ~DanmakuRendererItem → std::vector<DanmakuAtlasEntry>::_Tidy → _Orphan_all
#     （xmemory:1380，0xc0000005），
# 以及更早抓到过的 CRT 弹框 "HEAP CORRUPTION DETECTED … wrote to memory after end of heap buffer"。
# 也解释了"清空重建之后就不崩了"——但重建不是修复，只是碰巧让它重编了一次。
#
# 下面把这批头文件显式挂到那个生成物的对象依赖上，改成员布局后它会自动重编。
# ===========================================================================
set(_cicada_qml_registration_src
        "${CMAKE_CURRENT_BINARY_DIR}/appqtplayer_qmltyperegistrations.cpp")

file(GLOB _cicada_registered_headers
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

set_property(SOURCE "${_cicada_qml_registration_src}" APPEND PROPERTY
        OBJECT_DEPENDS "${_cicada_registered_headers}")

# ===========================================================================
# 弹幕**不需要**自定义着色器 / QSB（这里记一下为什么，别再往回改）
#
# 本轮先做的是一套"自定义 QSGMaterial + 自己用 qt_add_shaders() 烘焙 .qsb"的方案
# （顶点着色器里按 u_now 算弹幕位置、整屏合成一个 QSGGeometryNode）。材质、UBO 布局、
# 顶点属性寄存器、纹理提交全按 Qt 文档写对了，`qsb -d` 打开生成的 .qsb 也确认 6 个后端
# （含 HLSL 50）和反射信息齐全 —— 但 Qt 的 D3D11 后端运行时就是取不到那份 HLSL：
#     No HLSL (shader model 5.0) code found in baked shader QShader(stage=0 …)
#     HLSL shader compilation failed:
#     Failed to build graphics pipeline state
# 表现是弹幕整层不显示（实测：暂停后 grabWindow 前后比对，变化像素 0）。
#
# 现在改用 Qt 自带的 `QSGImageNode`（`QQuickWindow::createImageNode()`）：
# 每条弹幕一个节点，只画图集纹理里属于它的那块子矩形（setSourceRect 是**像素**坐标）。
# 着色器编在 Qt 库里、纹理上传也由 Qt 自己提交，这一类问题整个不存在。
#
# 所以：**不要再加回 qt_add_shaders()/shaders/*.vert|frag**，也不要再引入自定义
# QSGMaterial —— 除非换成非 D3D11 的后端并重新实测过。
# ===========================================================================

# 网页窗口（可选，见上面 CICADA_ENABLE_WEBVIEW 那段）：链 QtWebView 本体，
# 并给 C++ 一个编译期标志 —— main.cpp 里 QtWebView::initialize() 必须在引擎加载 QML
# 之前调用，但没开这个功能时不该依赖 QtWebView（也就不该去 include 它的头）。
# 平台后端（Windows 上是 WebView2）在下面 _cicada_static_image_plugins 里点名。
if (CICADA_ENABLE_WEBVIEW)
    target_link_libraries(appQtPlayer PRIVATE Qt6::WebView)
    target_compile_definitions(appQtPlayer PRIVATE CICADA_HAVE_WEBVIEW)
endif ()

# ===========================================================================
# 静态 Qt：把 QML 插件和平台/图片格式插件**显式链接进来**
#
# 动态 Qt 下 `import QtQuick` 是运行期去 qml/ 目录找 qtquick2plugin.dll；
# 静态 Qt 下没有这回事 —— 插件不会被自动发现，**必须链进去**，否则运行期报
# "module QtQuick is not installed" 这类错误（注意：这是运行期错，不是编译错，
# 所以"编过了"不等于"能跑"）。Qt 6 给的两个入口：
#
#   qt_import_qml_plugins(target)
#       跑 qmlimportscanner 扫这个 target 的 QML 导入清单，把对应的 QML 插件模块链进来。
#       本项目会命中：QtQuick、QtQuick.Controls.Basic（首页那几颗 Button）、
#       QtQuick.Dialogs（FileDialog/FolderDialog）、QtQuick.Effects（MultiEffect）、
#       **Qt.labs.lottieqt**（控制栏那 20 个 Lottie 图标，很多手搭的静态 Qt 会漏它）、
#       以及我们自己的 QtPlayer 模块。
#
#   qt_import_plugins(target INCLUDE <插件target>)
#       平台插件（qwindows）和图片格式插件（svg/webp/jpeg/gif/ico/tga/tiff/wbmp）——
#       这些不走 QML 导入清单，要自己点名。**qsvg 尤其重要**：素材里一堆 SVG。
#
# 【顺序】两个都必须在 target **finalize 之前**调用。qt_add_qml_module 的 finalize 是
# 延迟到当前目录作用域结束才执行的，所以写在这里（qt_add_qml_module 之后、文件末尾之前）
# 是对的。用 TARGET 判断逐个过滤：某个插件没装不该导致整个工程配不出来。
# ===========================================================================
if (CICADA_QT_STATIC)
    set(_cicada_static_image_plugins
            Qt6::QWindowsIntegrationPlugin
            Qt6::QSvgPlugin
            Qt6::QWebpPlugin
            Qt6::QJpegPlugin
            Qt6::QGifPlugin
            Qt6::QIcoPlugin
            Qt6::QTgaPlugin
            Qt6::QTiffPlugin
            Qt6::QWbmpPlugin
            # 网页窗口用的 WebView2 后端插件。QtWebView 本身只是个壳，真正的实现在
            # plugins/webview/ 下按平台选：Windows 上就是它（Edge WebView2 运行时，
            # Win10/11 自带）。它**不是 QML 插件**，所以 qt_import_qml_plugins 扫不到，
            # 必须在这里点名；静态构建下不链它，运行时 WebView 会创建失败。
            Qt6::QWebView2WebViewPlugin)

    set(_cicada_static_plugins_to_link "")

    foreach (_cicada_plugin IN LISTS _cicada_static_image_plugins)
        if (TARGET ${_cicada_plugin})
            list(APPEND _cicada_static_plugins_to_link ${_cicada_plugin})
        endif ()
    endforeach ()

    if (_cicada_static_plugins_to_link)
        qt_import_plugins(appQtPlayer INCLUDE ${_cicada_static_plugins_to_link})
    endif ()

    qt_import_qml_plugins(appQtPlayer)

    list(LENGTH _cicada_static_plugins_to_link _cicada_static_plugin_count)
    message(STATUS "QtPlayer: static Qt -> linked ${_cicada_static_plugin_count} platform/image "
                   "plugins + the QML plugins found by qmlimportscanner")

    unset(_cicada_static_image_plugins)
    unset(_cicada_static_plugins_to_link)
    unset(_cicada_plugin)
    unset(_cicada_static_plugin_count)
endif ()

target_include_directories(appQtPlayer PRIVATE
        ${CICADA_ROOT}/mediaPlayer
        ${CICADA_ROOT}/framework
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

# ---------------------------------------------------------------------------
# C++ 标准：**只有 Qt 这一侧要 17**
#
# Qt 6 硬性要求 C++17（QtCore/qcompilerdetection.h 里直接 #error "Qt requires a
# C++17 compiler"），而框架（mediaPlayer/demuxer/codec/render...）一直是按 C++11 编的
# （见 cmdline/CMakeLists.txt），所以：
#   * 上面的 CMAKE_CXX_STANDARD 11 保持不动 —— 框架那些子目录照旧；
#   * 本工程自己的源文件（要 include Qt 头）用目标属性覆盖成 17。
#
# MSVC 上还要 /Zc:__cplusplus 才能让 __cplusplus 反映出真实标准，这个选项由
# Qt6::Platform（Qt6::Quick 传递进来）自动加上，见 Qt6Targets.cmake。
# OBJCXX_STANDARD 一起设，macOS 上的 .mm 也走 17。
# ---------------------------------------------------------------------------
set_target_properties(appQtPlayer PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        OBJCXX_STANDARD 17
        OBJCXX_STANDARD_REQUIRED ON
        )

# ---------------------------------------------------------------------------
# Qt 6.5 ~ 6.7 的原生纹理兜底路线要用 QRhi（见 src/CicadaQtTextureWrap.cpp）：
# rhi/qrhi.h 放在 Qt 的"版本化头目录"里，只有链了 Qt6::GuiPrivate 才会出现在包含
# 路径上。Qt 6.8 及以上走 QNativeInterface::QSG*Texture::fromNative()，不需要它。
#
# 这里是 QUIET 探测：装了私有头就链上（兜底路线生效），没装就什么都不做
# （源码里用 __has_include 自动编成"没有这条路线"，仍然能编能跑 CPU 路径）。
# ---------------------------------------------------------------------------
if (Qt6_VERSION VERSION_LESS 6.8)
    find_package(Qt6GuiPrivate QUIET)

    if (TARGET Qt6::GuiPrivate)
        target_link_libraries(appQtPlayer PRIVATE Qt6::GuiPrivate)
        message(STATUS "QtPlayer: Qt ${Qt6_VERSION} < 6.8, linking Qt6::GuiPrivate "
                       "so the QRhi native-texture fallback is available")
    else ()
        message(STATUS "QtPlayer: Qt ${Qt6_VERSION} < 6.8 and Qt6::GuiPrivate is not "
                       "installed; the native-texture fallback is compiled out "
                       "(video goes through the CPU path)")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# 应用级对话框（src/AppFileDialogs.*）：**从 C++ 打开系统原生的文件/目录选择框**
#
# 为什么需要 QtGui 私有头：QtGui 里那层平台对话框抽象
#   QPlatformTheme::createPlatformDialogHelper(QPlatformTheme::FileDialog)
# 声明在 <QtGui/qpa/qplatformtheme.h>，而 qpa/ 和 private/ 两个包含目录只有链了
# Qt6::GuiPrivate 才会出现（Qt6::Gui 只导出 include/QtGui 和 include）。所以这里
# 单独探测一次：装了私有头 → 编出"原生对话框"那条路（CICADA_QT_HAVE_PLATFORM_FILE_DIALOG）；
# 没装 → AppFileDialogs 退化成"不可用"，QML 自动回退到 QtQuick.Dialogs 的 FileDialog。
#
# 和上面那段不是一回事（那段只在 Qt < 6.8 时为了 QRhi 才找私有头），所以这里
# 无条件找一次；TARGET 判断保证重复 find_package 也不会重复链接。
# ---------------------------------------------------------------------------
if (NOT TARGET Qt6::GuiPrivate)
    find_package(Qt6GuiPrivate QUIET)
endif ()

if (TARGET Qt6::GuiPrivate)
    target_link_libraries(appQtPlayer PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(appQtPlayer PRIVATE CICADA_QT_HAVE_PLATFORM_FILE_DIALOG=1)
    message(STATUS "QtPlayer: native file dialogs from C++ enabled (Qt6::GuiPrivate found)")
else ()
    message(STATUS "QtPlayer: Qt6::GuiPrivate not found — file dialogs fall back to QML "
                   "QtQuick.Dialogs (handles are then released by the QML destroy path)")
endif ()

target_link_directories(appQtPlayer PRIVATE ${COMMON_LIB_DIR})

if (WIN32)
    # 崩溃时打印调用栈用的 dbghelp（见 main.cpp 里 cicadaUnhandledException 的说明）。
    # Windows SDK 自带，不用 find_package。
    target_link_libraries(appQtPlayer PRIVATE dbghelp)
endif ()

target_link_libraries(appQtPlayer PRIVATE
        Qt6::Quick
        # 实时弹幕：DanmakuView.qml 里 `import QtWebSockets`（mock 的 /danmaku/ws）。
        # 静态构建下 QML 插件由 qt_import_qml_plugins 扫描 QML_FILES 里的 import 自动链上，
        # 这里显式链一次模块本身，避免只在运行时才发现缺 QtWebSockets。
        Qt6::WebSockets
        # 独立弹幕引擎（纯 C++，见上面的 add_subdirectory）
        Danmaku
        media_player
        demuxer
        data_source
        render
        videodec
        framework_filter
        framework_utils
        framework_drm
        )

if (ENABLE_CACHE_MODULE)
    target_link_libraries(appQtPlayer PRIVATE cacheModule)
endif ()
if (ENABLE_MUXER)
    target_link_libraries(appQtPlayer PRIVATE muxer)
endif ()

# ---------------------------------------------------------------------------
# FFmpeg
#
# Windows 上链的是合并库 libffmpeg.lib（名字必须写成 libffmpeg，磁盘上就是这个名字），
# 运行时要把 libffmpeg.dll 放到 exe 旁边；其它平台按模块链。
# ---------------------------------------------------------------------------
if (MSVC)
    target_link_libraries(appQtPlayer PRIVATE libffmpeg)
else ()
    target_link_libraries(appQtPlayer PRIVATE
            avfilter
            avformat
            avcodec
            swresample
            swscale
            avutil)
endif ()

target_link_libraries(appQtPlayer PRIVATE ${FRAMEWORK_LIBS})

# 无边框窗口（QWindowKit）。静态库，链接进来即可；QML 类型由 main.cpp 里调一次
# QWK::registerTypes(engine) 注册（它内部是 qmlRegisterType<QuickWindowAgent>
# ("QWindowKit", 1, 0, "WindowAgent")，不需要 qmldir 或 QML 插件、也不需要导入路径）。
target_link_libraries(appQtPlayer PRIVATE QWindowKit::Quick)

if (NOT MSVC)
    target_link_libraries(appQtPlayer PRIVATE
            curl
            nghttp2
            xml2)
endif ()

# ---------------------------------------------------------------------------
# 音频
#
# 本轮视频全部交给 Qt，音频沿用框架现有的音频渲染器：
#   * Windows/Linux：framework/render/audio/SdlAFAudioRender2（需要 SDL2，只用于音频，
#     不会建窗口 —— 视频渲染器已经被我们注入的 CicadaVideoRender 顶掉了）；
#   * macOS：AFAudioQueueRender（系统原生，不需要 SDL）。
# AV 同步是框架现成的（以音频时钟为主时钟），所以这里保留原样最省事。
# ---------------------------------------------------------------------------
if (ENABLE_SDL AND NOT MSVC)
    if (CICADA_SDL2_LIBRARIES)
        if (CICADA_SDL2_LIBRARY_DIRS)
            target_link_directories(appQtPlayer PRIVATE ${CICADA_SDL2_LIBRARY_DIRS})
        endif ()
        target_link_libraries(appQtPlayer PUBLIC ${CICADA_SDL2_LIBRARIES})
    else ()
        target_link_libraries(appQtPlayer PUBLIC SDL2)
    endif ()
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    target_link_libraries(appQtPlayer PUBLIC
            bcrypt
            ole32
            # 零拷贝：ID3D11VideoProcessor 在 d3d11，DXGI 在 dxgi
            d3d11
            dxgi
            )
else ()
    target_link_libraries(appQtPlayer PUBLIC
            z
            dl
            )
endif ()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_link_libraries(appQtPlayer PUBLIC bz2)
endif ()

if (APPLE)
    find_library(AVFOUNDATION AVFoundation)
    find_library(QUARTZCORE QuartzCore)
    # macOS 零拷贝后端（下一轮补完）要用到 VideoToolbox / CoreVideo：
    #   VTPixelTransferSession（NV12 -> BGRA）与 CVMetalTextureCache。
    find_library(VIDEOTOOLBOX VideoToolbox)
    find_library(COREVIDEO CoreVideo)
    # 注：macOS 的视频色彩调整**不用** Core Image —— 走的是 src/CicadaTextureMetal.mm 里的
    # Metal 计算内核。前两次用 Core Image 的方案在真机上都变成"一拖滑块整屏红"（原因见那个
    # 文件的说明），所以这里**不再**需要 -framework CoreImage。
    target_link_libraries(appQtPlayer PUBLIC
            iconv
            bz2
            ${FRAMEWORK_LIBS}
            ${AVFOUNDATION}
            ${QUARTZCORE}
            ${VIDEOTOOLBOX}
            ${COREVIDEO}
            )
elseif (MSVC)
    # 什么都不加
else ()
    target_link_libraries(appQtPlayer PUBLIC
            ssl
            crypto
            pthread
            )
endif ()

target_link_libraries(appQtPlayer PUBLIC coverage_config)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    target_link_libraries(appQtPlayer PUBLIC X11)
    # VAAPI 零拷贝后端要用 libva / libva-drm / EGL（见 src/CicadaTextureVAAPI.cpp）；
    # 这里沿用 Linux.cmake 探测出来的结果，没装 libva-dev 的开发机也能编过。
    if (CICADA_VAAPI_LIBS)
        target_link_libraries(appQtPlayer PRIVATE ${CICADA_VAAPI_LIBS})
    endif ()

    # 只有 libva 和 EGL 的头 + EGL 库都齐了才打开 CICADA_QT_HAVE_VAAPI；
    # 否则 CicadaTextureVAAPI.cpp 整体退化成"这条后端不可用"，工程照样能编能跑
    # （视频走 CPU 回退路径）。
    find_path(CICADA_QT_VAAPI_INCLUDE_DIR va/va.h)
    find_path(CICADA_QT_EGL_INCLUDE_DIR EGL/egl.h)
    find_library(CICADA_QT_EGL_LIBRARY EGL)
    if (CICADA_QT_VAAPI_INCLUDE_DIR AND CICADA_QT_EGL_INCLUDE_DIR AND CICADA_QT_EGL_LIBRARY)
        target_compile_definitions(appQtPlayer PRIVATE CICADA_QT_HAVE_VAAPI=1)
        target_link_libraries(appQtPlayer PRIVATE ${CICADA_QT_EGL_LIBRARY})
        message(STATUS "QtPlayer: VAAPI/EGL zero-copy backend enabled")
    else ()
        message(STATUS "QtPlayer: libva or EGL headers not found, the VAAPI zero-copy "
                       "backend is compiled out (video goes through the CPU path)")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# 单元测试（**默认关**：它不影响主程序，但没验证过之前不想拖累正常构建；
# 要跑测试就配置时打开：-DCICADA_QT_BUILD_TESTS=ON）
#
# 测的是**核心播放器**接受对象这件事，不是应用层：
#   * framework/demuxer/manifest/MediaManifestParser.cpp ——
#     MediaPlayer::SetDataSource(const std::string &jsonManifest) 内部第一步，
#     把参考实现给的对象（dist/test-*.json）解析成 MediaManifest。
#     分片展开在 ManifestDemuxer::buildPlayList() 里（要 playList 内部状态），
#     不在这个纯数据测试范围内。
#     URL 模式（SetDataSource(const char *)）不经过这里，所以不在测试范围。
# 跑法：
#     cmake -S . -B build\msvc -DCICADA_QT_BUILD_TESTS=ON ...
#     cmake --build build\msvc --config Release --target testObjectManifest
#     build\msvc\Release\testObjectManifest.exe
# ---------------------------------------------------------------------------
option(CICADA_QT_BUILD_TESTS "Build QtPlayer unit tests" OFF)

if (CICADA_QT_BUILD_TESTS)
    # Qt6::Test 是独立组件，单独找一下；找不到就跳过测试（不影响主程序）。
    find_package(Qt6 QUIET COMPONENTS Test)

    if (TARGET Qt6::Test)
        # 测试数据：参考工程 front/hili-player/dist 下的 test-*.json
        # （就是"喂给播放器的那种对象"）。这里用相对源码目录的路径推出来，
        # 不写死任何机器上的绝对路径。
        get_filename_component(CICADA_QT_REFERENCE_DIST
                ${CMAKE_CURRENT_SOURCE_DIR}/../../../front/hili-player/dist ABSOLUTE)

        add_executable(testObjectManifest tests/testObjectManifest.cpp)
        # 测试代码要 include 框架的头（<demuxer/manifest/MediaManifestParser.h>），
        # 框架的静态库没把包含目录挂在接口上，这里显式加一次。
        target_include_directories(testObjectManifest PRIVATE ${CICADA_ROOT}/framework)
        target_link_libraries(testObjectManifest PRIVATE
                # demuxer 里就是 MediaManifestParser.cpp / ManifestDemuxer.cpp；
                # framework_utils 提供 CicadaJSON 和日志。
                demuxer framework_utils Qt6::Test)
        target_compile_definitions(testObjectManifest PRIVATE
                CICADA_QT_REFERENCE_DIST="${CICADA_QT_REFERENCE_DIST}")
        set_target_properties(testObjectManifest PROPERTIES
                CXX_STANDARD 17
                CXX_STANDARD_REQUIRED ON)

        enable_testing()
        add_test(NAME testObjectManifest COMMAND testObjectManifest)
        message(STATUS "QtPlayer: unit tests enabled (testObjectManifest), "
                       "reference objects: ${CICADA_QT_REFERENCE_DIST}")
    else ()
        message(STATUS "QtPlayer: Qt6::Test not found, unit tests are skipped")
    endif ()
endif ()

# ---------------------------------------------------------------------------
# 可执行文件属性
# ---------------------------------------------------------------------------
# 32 位 MSVC（Win32 交叉构建）下 vcpkg 里有些静态库带 /SAFESEH 不兼容的异常处理，
# cmdline 里也是这么关掉的（否则链接报 LNK2026）。
if (MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /SAFESEH:NO")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /SAFESEH:NO")
endif ()

set_target_properties(appQtPlayer PROPERTIES
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        MACOSX_BUNDLE TRUE
        )

# ---------------------------------------------------------------------------
# 控制台窗口：Debug 留着，Release 关掉（发布版双击不该弹黑框）
#
# 为什么不用 WIN32_EXECUTABLE 属性：它**不能按配置区分** —— Visual Studio 这类多配置
# 生成器上，属性一设就是所有配置都变成 GUI 子系统。所以按配置直接给链接器下发子系统
# 开关（生成器表达式在链接选项里是支持的，多配置下各配置各拿各的）：
#
#   Debug   -> /SUBSYSTEM:CONSOLE
#              保留控制台：框架的日志是 printf 到 stdout 的（见 frame_work_log.c），
#              排查"到底走没走零拷贝 / seek 卡在哪一步"时直接就能看见。
#
#   其它    -> /SUBSYSTEM:WINDOWS + /ENTRY:mainCRTStartup
#              GUI 子系统（不弹控制台），入口仍然是 main()：显式指定 mainCRTStartup
#              就不需要 WinMain，也不用指望 Qt6::EntryPoint 被链上。
#
# 注意两件事：
#   * Release 下没有控制台，printf 的日志不会显示。需要日志就用 Debug 配置，
#     或者重定向：`appQtPlayer.exe > log.txt 2>&1`（stdout 句柄由命令行给，仍能落盘）；
#   * 这套只对 MSVC 做。MinGW 上等价的开关是 -mwindows，需要时再加。
# ---------------------------------------------------------------------------
if (MSVC)
    target_link_options(appQtPlayer PRIVATE
            "$<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>"
            "$<$<NOT:$<CONFIG:Debug>>:/SUBSYSTEM:WINDOWS>"
            "$<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>")
endif ()

# ---------------------------------------------------------------------------
# 弹幕引擎的 DLL 放到 exe 旁边
#
# appQtPlayer 链接的是 Danmaku（共享库），但它的二进制在**另一个目录**
# （build/msvc/danmaku.out/...），运行时 exe 找不到它 → 起来就 0xC0000135。
# 所以照 libffmpeg.dll 那套：POST_BUILD 拷一份到 exe 旁，install 也带一份。
# ---------------------------------------------------------------------------
if (TARGET Danmaku AND WIN32)
    add_custom_command(TARGET appQtPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:Danmaku>
                    $<TARGET_FILE_DIR:appQtPlayer>
            COMMENT "copying Danmaku.dll next to appQtPlayer.exe")

    install(FILES $<TARGET_FILE:Danmaku>
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

install(TARGETS appQtPlayer
        BUNDLE DESTINATION .
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )

# ---------------------------------------------------------------------------
# 部署：让 `cmake --install` 装出一棵能直接运行的目录树
#
# **这是个 QML 应用，必须用 qt_generate_deploy_qml_app_script()** —— 它比
# qt_generate_deploy_app_script() 多做一件事：内部调用 qt_deploy_qml_imports()，
# 按构建期 qmlimportscanner 扫出来的导入清单把 **QML 模块**一起部署。
# 用错函数（本项目最初就是）的表现：Qt 的 DLL、plugins/、translations/ 都装好了，
# 唯独没有 qml/ 目录、也没有 Qt6QuickDialogs2.dll —— 程序起来加载 Main.qml 时
# `import QtQuick` 失败，静默退出。导入清单就在
# build/<dir>/.qt/qml_imports/appQtPlayer_build.cmake，可以自己翻。
#
# 目录布局（Qt 的约定，别改）：可执行文件进 CMAKE_INSTALL_BINDIR（= bin），
# Qt 的运行库跟着 exe 放进 bin/，plugins/ 和 qml/ 放在前缀根目录，
# exe 旁边再生成一个 qt.conf 指回去。所以**框架自己的 DLL 也必须装到同一个 bin/**，
# 否则 exe 找不到 libffmpeg.dll（踩过：装到了前缀根目录，一边一个）。
#
# 用法（Qt Creator 的部署步骤走的也是这条路）：
#     cmake --install build\msvc --config Release --prefix deploy
# 或者直接用本目录的 deploy_win.bat（编译 + install + 依赖自检一条龙）。
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 【静态 Qt 不做这一步】部署脚本干的事是"按导入清单把 Qt 的运行库、qml/ 模块、
# platforms/ 插件拷到安装目录旁边"—— 静态 Qt 里这些东西**被链进 exe 了**，
# 没有可拷的。所以在静态构建下整段跳过（调了也没东西可拷，还会让部署脚本去
# 找不存在的 Qt6*.dll）。框架自己的 DLL（libffmpeg / Danmaku / vcpkg 那几个）
# 在下面那段里照旧拷 —— 那些不是 Qt。
# ---------------------------------------------------------------------------
if (NOT CICADA_QT_STATIC)
    qt_generate_deploy_qml_app_script(
            TARGET appQtPlayer
            OUTPUT_SCRIPT cicada_qt_deploy_script
            # macOS：构建完（POST_BUILD）就往 .app 里部署一遍，这样 build_macos.sh
            # 不加 --deploy 也能直接双击跑；其它平台忽略这个选项。
            MACOS_BUNDLE_POST_BUILD
            # Linux 上 Qt 一般在系统路径里，没有"部署"这回事，失败也无所谓。
            NO_UNSUPPORTED_PLATFORM_ERROR
            # 不支持"部署"的平台上，也要把应用自己的 QML 模块带上（否则 Linux 上
            # 那些自定义 QML 模块会缺）。
            DEPLOY_USER_QML_MODULES_ON_UNSUPPORTED_PLATFORM
    )

    install(SCRIPT ${cicada_qt_deploy_script})
else ()
    message(STATUS "QtPlayer: static Qt -> skipping the Qt deploy script "
                   "(nothing to copy, Qt is linked into the executable)")
endif ()

# ---------------------------------------------------------------------------
# Windows 运行时依赖（拷到 exe 旁边）
#
#   * libffmpeg.dll：win32 交叉构建的合并库，build_win.bat 里也是这么拷的；
#   * pthread / SDL2 的 DLL：来自 vcpkg。走 vcpkg 工具链时它自己的 applocal 会拷，
#     我们这里没走工具链（见文件开头说明），所以显式拷一遍 —— 只用框架真正链接的
#     两个，路径和 vcpkg applocal 一致：Release 在 installed/<triplet>/bin，
#     Debug 在 installed/<triplet>/debug/bin。
# ---------------------------------------------------------------------------
if (WIN32 AND MSVC)
    set(_cicada_ffmpeg_dll "${CICADA_ROOT}/external/install/ffmpeg/win32/${ARCH}/libffmpeg.dll")

    if (EXISTS "${_cicada_ffmpeg_dll}")
        add_custom_command(TARGET appQtPlayer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_cicada_ffmpeg_dll}" "$<TARGET_FILE_DIR:appQtPlayer>"
                COMMENT "copying libffmpeg.dll next to appQtPlayer.exe")
        # 部署（cmake --install）出来的那棵树也得有它，不然脱离 build 目录就起不来。
        #
        # 目的地必须和可执行文件**完全一致**：exe 装到 ${CMAKE_INSTALL_BINDIR}（= bin），
        # Qt 的运行库也被 Qt 的部署脚本放到 exe 旁边（同一个 bin/），所以框架自己的
        # libffmpeg.dll 也必须进 bin/。装到前缀根目录的话 exe 就找不到它
        # （踩过一次：-- Installing 打印的是 deploy/./libffmpeg.dll，而 exe 在 deploy/bin/）。
        install(FILES "${_cicada_ffmpeg_dll}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    else ()
        message(STATUS "QtPlayer: ${_cicada_ffmpeg_dll} not found; copy libffmpeg.dll "
                       "next to the exe yourself before running")
    endif ()

    # -----------------------------------------------------------------------
    # 框架在 Windows/MSVC 上依赖的 vcpkg **运行时** DLL —— 缺一个换台机器就起不来。
    #
    #   * pthreadVC3/VCE3/VSE3   PThreads4W（框架的线程封装）
    #   * SDL2 / SDL2main        框架的音频渲染器（SdlAFAudioRender2）
    #   * libxml2 那一串         framework/utils/xml/DOMParser 用 libxml2；
    #                            libxml2.dll 自己还依赖 zlib1 / iconv-2 / charset-1
    #                            —— 换台机器报"缺少 libxml2.dll"，就是只拷了
    #                            pthread/SDL2、没拷这条依赖链
    #   * libcurl 那一串         网络源（curl + nghttp2 + brotli）
    #   * libssl / libcrypto     curl 的 https
    #
    # 列表写宽一点没关系：下面每项都带 if (EXISTS ...) 过滤，不存在的不拷。
    # 对照：cmdline 自己的 msvc_build 拷的就是 zlib1 / iconv-2 / libxml2 / SDL2 / pthreadVC3。
    # -----------------------------------------------------------------------
    set(CICADA_VCPKG_RUNTIME_DLLS
            pthreadVC3.dll pthreadVCE3.dll pthreadVSE3.dll
            SDL2.dll SDL2main.dll
            libxml2.dll zlib1.dll iconv-2.dll charset-1.dll
            libcurl.dll nghttp2.dll brotli.dll brotlidec.dll brotlicommon.dll
            libssl-3-x64.dll libcrypto-3-x64.dll
            )

    if (CICADA_VCPKG_ROOT)
        set(_cicada_vcpkg_bin
                "${CICADA_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}$<$<CONFIG:Debug>:/debug>/bin")

        foreach (_cicada_dll IN LISTS CICADA_VCPKG_RUNTIME_DLLS)
            if (EXISTS "${_cicada_vcpkg_bin}/${_cicada_dll}")
                add_custom_command(TARGET appQtPlayer POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                "${_cicada_vcpkg_bin}/${_cicada_dll}"
                                "$<TARGET_FILE_DIR:appQtPlayer>")
            endif ()
        endforeach ()

        unset(_cicada_vcpkg_bin)
    endif ()

    # -----------------------------------------------------------------------
    # 部署目录里放的是 **Release** 那一套 vcpkg DLL：
    #   * 上面 POST_BUILD 那次是给"本机直接跑 build 目录里的 exe"用的（Debug 版就拷
    #     installed/<triplet>/debug/bin）；
    #   * 部署是给别人/别的机器用的，Debug 版 Qt DLL 本来就不可分发（还要求目标机器
    #     装 VC++ 调试运行时），所以这里固定用 installed/<triplet>/bin。
    # 注意：这里**不能**用 $<CONFIG:Debug> 这类生成器表达式 —— if(EXISTS ...) 在配置期
    # 求值，表达式不会被展开，条件会永远为假（上面那个 POST_BUILD 循环就有这个毛病，
    # 只是恰好不影响运行）。
    # -----------------------------------------------------------------------
    if (CICADA_VCPKG_ROOT)
        set(_cicada_deploy_bin "${CICADA_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/bin")

        foreach (_cicada_dll IN LISTS CICADA_VCPKG_RUNTIME_DLLS)
            if (EXISTS "${_cicada_deploy_bin}/${_cicada_dll}")
                # 同样进 bin/：必须和 exe、libffmpeg.dll 在同一个目录。
                install(FILES "${_cicada_deploy_bin}/${_cicada_dll}"
                        DESTINATION ${CMAKE_INSTALL_BINDIR})
            endif ()
        endforeach ()

        unset(_cicada_deploy_bin)
    endif ()

    unset(_cicada_ffmpeg_dll)
endif ()
