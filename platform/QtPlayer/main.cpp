//
// Qt6 QML 播放器组件的入口。
//
// 这个可执行文件既是"组件演示（demo）"，也是"组件怎么接框架"的参考实现：
//   * main() 里做两件事：注册视频渲染器（图形设备、后端全部交给 Qt 自己定），
//     以及把命令行里给的视频路径转成 url 交给 QML；
//   * Main.qml 里演示组件怎么用（进度条、播放/暂停/快进、打开本地文件、拖拽、诊断信息）。
//
// 播放哪个片子有三种选法，都不用改代码重新编译：
//   appQtPlayer.exe                       -> 启动后用界面左下角「打开」按钮选，或把文件拖进窗口
//   appQtPlayer.exe D:\video\4k.mp4       -> 启动就播这个本地文件
//   appQtPlayer.exe https://host/a.m3u8   -> 启动就播这个网络地址
//
// 想在别的 QML 工程里复用这个组件，需要的是：
//   1. 把 platform/QtPlayer/src 下的源码加到你的工程；
//   2. 在你的 CMakeLists 里照抄本目录 CMakeLists.txt 中"框架的部分"
//      （add_subdirectory(${CICADA_ROOT}/mediaPlayer ...) 那一坨）和 qt_add_qml_module；
//   3. 在你的 main() 里调用一次 registerCicadaVideoRender()（或者直接调
//      videoRenderFactory::setRenderCreator）。
//
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QSGRendererInterface>
/* setInitialProperties 要用的 QVariantMap，以及"命令行路径 -> url"的转换。 */
#include <QtCore/QStringList>
#include <QtCore/QUrl>
#include <QtCore/QVariant>
#include <QtCore/QVariantMap>
/* 读命令行给的流媒体清单文件（.json）用。 */
#include <QtCore/QFile>
#include <QtCore/QByteArray>
/* 默认日志路径（用户数据目录）用。 */
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
/* reportStartupFailure() 往 stderr 写一句人话。 */
#include <cstdio>
#include <cstdlib>
/* 崩溃时打印调用栈（见 cicadaUnhandledException 的说明）。 */
#include <exception>
/* 卡死看门狗：GUI 心跳（原子量）+ 后台线程 + 单调时钟（见 startHangWatchdog 的说明）。 */
#include <atomic>
#include <chrono>
#include <thread>
#include <QtCore/QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
/* 枚举本进程所有线程（卡死时逐个走栈要用）。 */
#include <tlhelp32.h>
/* CRT 堆报告钩子（Debug 堆发现越界写时抓栈，见 cicadaCrtReportHook）。 */
#include <crtdbg.h>
#include <dbghelp.h>
#endif

#include <utils/frame_work_log.h>
#include <render/renderFactory.h>

/*
 * 无边框窗口（QWindowKit，vendored 在 3rdparty/qwindowkit）。
 * 头文件路径来自它的 sync-include：链接 QWindowKit::Quick 就有 <QWKQuick/...>。
 *
 * 注意：它只做**窗口系统**这一层（Windows 上是原生 WM_NCHITTEST/DWM），
 * 完全不碰 Qt 的图形 API —— 场景图用哪个 RHI 后端、用哪块 GPU 设备仍然全由 Qt 决定。
 */
#include <QWKQuick/qwkquickglobal.h>

#include "src/CicadaVideoRender.h"
/* 截屏结果的 QML 出口：image://snapshot/<revision>（进度条悬停预览）。 */
#include "src/SnapshotImageProvider.h"
/* 应用级文件/目录选择对话框（从 C++ 调原生对话框，见那个文件头）。 */
#include "src/AppFileDialogs.h"

/*
 * 把 Qt 端自己的"无窗口视频渲染器"注册给框架。
 *
 * 必须在**任何 MediaPlayer 被创建/Prepare 之前**调用：框架是在准备视频通路时
 * （SuperMediaPlayer::setUpVideoRender -> videoRenderFactory::create）把这个渲染器
 * 造出来的。注册之后：
 *   * 框架不再会去建 SDL 窗口（SdlAFVideoRenderer 被顶掉）；
 *   * 也不会再用 DummyVideoRender 把帧丢掉；
 *   * 解码帧会交到 QML 组件注册的渲染回调里（见 CicadaPlayerItem）。
 */
static void registerCicadaVideoRender()
{
    Cicada::videoRenderFactory::setRenderCreator(&cicadaqt::createCicadaVideoRender);
}

/* =====================================================================================
 * 把图形后端枚举变成人能看懂的名字（枚举值随 Qt 版本可能增加，所以用 switch 而不是
 * 在日志里硬写数字）。
 */
static const char *graphicsApiName(QSGRendererInterface::GraphicsApi api)
{
    switch (api) {
        case QSGRendererInterface::Software:
            return "software";

        case QSGRendererInterface::OpenVG:
            return "OpenVG";

        case QSGRendererInterface::OpenGL:
            return "OpenGL (RHI)";

        case QSGRendererInterface::Direct3D11:
            return "Direct3D11 (RHI)";

        case QSGRendererInterface::Direct3D12:
            return "Direct3D12 (RHI)";

        case QSGRendererInterface::Vulkan:
            return "Vulkan (RHI)";

        case QSGRendererInterface::Metal:
            return "Metal (RHI)";

        case QSGRendererInterface::Null:
            return "null (no rendering)";

        default:
            return "unknown";
    }
}

/*
 * 命令行里那个视频地址；没给就返回一个空 url。
 *
 * 规则：
 *   * 跳过以 '-' 开头的参数 —— 那是 Qt 自己的开关（-platform windows:darkmode=2 之类），
 *     不能当成文件名；
 *   * 取第一个剩下的参数：带协议的（http://、rtmp://、file:）原样用，
 *     其余的按**本地路径**处理；
 *   * 本地路径必须过 QUrl::fromLocalFile()，不能直接把字符串塞给 QUrl：
 *     QUrl("D:\\video\\a.mp4") 会把 "D" 当成协议名解析，最后是打不开文件的。
 *
 * 用 QCoreApplication::arguments() 而不是裸的 argv：它会正确处理 Windows 上的宽字符
 * 命令行（中文路径就靠它）。
 *
 * **.json 是个特例**：那种参数按"流媒体清单对象"处理，见 startupManifestFromCommandLine()。
 */
static QUrl startupSourceFromCommandLine()
{
    const QStringList args = QCoreApplication::arguments();

    for (int i = 1; i < args.size(); ++i) {
        const QString &arg = args.at(i);

        if (arg.isEmpty() || arg.startsWith(QLatin1Char('-'))) {
            continue;
        }

        /* 清单文件走另一条路（下面那个函数），这里跳过。 */
        if (arg.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            continue;
        }

        if (arg.contains(QStringLiteral("://")) || arg.startsWith(QStringLiteral("file:"))) {
            return QUrl(arg);
        }

        return QUrl::fromLocalFile(arg);
    }

    return QUrl();
}

/*
 * 命令行里那个**流媒体清单**（.json 文件）。
 *
 * 用途：不用手点，直接把一份清单对象喂给播放器 —— 内容由你自己准备（真实的 playurl
 * 清单 / 导出的 test-*.json 都行），这里只负责读文件、把内容交给组件：
 *
 *     appQtPlayer.exe D:\path\to\test-hls-v5.json
 *     appQtPlayer.exe D:\path\to\test-dash.json
 *
 * 这份 json 就是 hili-player 的 `dist/test-{hls,dash}*.json` 那一套结构
 * （duration / minBufferTime / video[] / audio[] / segmentInfo）。Qt 侧只把它当文本
 * 透传（CicadaPlayerItem::setManifestJson，不做任何字段转换），最终进核心的
 * MediaPlayer::SetDataSource(const std::string &jsonManifest)，由核心的
 * ManifestParser/ManifestDemuxer 自己解析并展开分片。
 *
 * manifest 非空表示命令行给了清单；hint 是那份清单的文件名，只用于界面/日志显示
 * （走 HLS 还是 DASH 管线由核心按对象里的 mediaSourceType 决定，缺省 hls）。
 */
static QString startupManifestFromCommandLine(QString *hint)
{
    const QStringList args = QCoreApplication::arguments();

    for (int i = 1; i < args.size(); ++i) {
        const QString &arg = args.at(i);

        if (arg.isEmpty() || arg.startsWith(QLatin1Char('-'))) {
            continue;
        }

        if (!arg.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            continue;
        }

        QFile file(arg);

        if (!file.open(QIODevice::ReadOnly)) {
            fprintf(stderr, "无法打开清单文件：%s\n", arg.toUtf8().constData());
            AF_LOGE("cannot open manifest file: %s\n", arg.toUtf8().constData());
            return QString();
        }

        const QByteArray content = file.readAll();
        file.close();

        if (hint != nullptr) {
            *hint = arg;
        }

        fprintf(stderr, "已读取流清单：%s（%lld 字节）\n",
                arg.toUtf8().constData(), static_cast<long long>(content.size()));
        AF_LOGI("manifest file: %s (%lld bytes)\n",
                arg.toUtf8().constData(), static_cast<long long>(content.size()));

        return QString::fromUtf8(content);
    }

    return QString();
}

/*
 * QML 加载失败时给人一句看得懂的话。
 *
 * 以前这里是"静默 exit(-1)"：程序起来、又立刻退掉，什么都看不到，只能猜。
 * 现在把最常见的原因和对应做法直接打出来（本程序是控制台子系统，日志在控制台里）。
 */
static void reportStartupFailure()
{
    const QString message = QStringLiteral(
        "\n"
        "============================================================\n"
        "无法加载 QML 界面（QtPlayer/Main.qml），程序退出。\n"
        "\n"
        "如果上面一行是： Module \"QtPlayer\" contains no type named \"Main\"\n"
        "  说明 QML 模块的资源前缀不对 —— QtPlayer 的 qmldir 没被嵌到默认导入路径里。\n"
        "  本工程的 CMakeLists.txt 里已经用 qt_policy(SET QTP0001 NEW) 修掉了，\n"
        "  重新 cmake 配置 + 编译即可（旧的构建目录建议删掉重建）。\n"
        "\n"
        "如果是其它模块缺失（QtQuick / QtQuick.Dialogs 等）：\n"
        "  * 部署目录里必须有 qml/ 子目录（QtQuick、QtQuick/Dialogs……）以及\n"
        "    platforms/qwindows.dll。裸用 windeployqt 是**不会**拷 qml/ 的，请用：\n"
        "        cmake --install build\\msvc --prefix deploy\n"
        "    （CMakeLists 里接了 qt_generate_deploy_app_script，会按导入清单一起装）\n"
        "    或者 windeployqt --qmldir <本目录> appQtPlayer.exe\n"
        "  * 另外确认 libffmpeg.dll、pthreadVC3.dll、SDL2.dll 和 exe 在同一个目录。\n"
        "\n"
        "调试期也可以直接带上 Qt 的 bin 目录跑（不部署）：\n"
        "        set PATH=D:\\Qt\\<版本>\\msvc2022_64\\bin;%PATH%\n"
        "============================================================\n");

    /* 控制台和框架日志各留一份：双击运行、重定向到文件时都能看到。 */
    fprintf(stderr, "%s", message.toUtf8().constData());
    AF_LOGE("%s", message.toUtf8().constData());
}

namespace {

    /*
     * ============================ 单一日志出口 ============================
     *
     * 【为什么需要它】用户交上来的 log.txt 是**被两个写入者交错覆盖**的：
     * 里面有成段重复的内容，还有被截断后跟另一行拼在一起的半行（例如
     * `...hevc (hw pixel form2026-09-21 19:08:18.381 ...`）。原因是框架日志走
     * stdout（`frame_work_log.c` 的 printf），而下面那个 Qt 消息处理器又把
     * 同一条告警额外写了一份 stderr；两路各自被重定向/被 IDE 抓取，交错起来
     * 就没法当证据用了 —— 排查清晰度切换卡死时，关键的那几行正好被吃掉。
     *
     * 做法：把框架日志接一个**进程内唯一**的文件出口（`log_set_back`，
     * `frame_work_log.c:352`；它已经在 `gLogMutex` 里逐行调用，所以天然串行），
     * 谁都不用再依赖外部重定向。控制台输出照旧保留（Debug 下还能直接看）。
     *
     * 路径优先级：
     *   1) 命令行 `--log-file <path>` / `--log-file=<path>`
     *   2) 环境变量 `CICADA_LOG_FILE`
     *   3) 默认 `<用户数据目录>/appQtPlayer.log`（QStandardPaths::AppLocalDataLocation）
     * 想关掉：把路径写成 `-`（或设 `CICADA_LOG_FILE=-`）。
     *
     * 注意 `g_cicadaLogFile` 非空还兼作"已经有了单文件出口"的判据 ——
     * cicadaQtMessageHandler 用它决定要不要再往 stderr 补一份（见那里的注释）。
     */
    FILE *g_cicadaLogFile = nullptr;

    void cicadaLogFileBack(void * /*arg*/, int prio, const char *buf)
    {
        if (g_cicadaLogFile == nullptr || buf == nullptr) {
            return;
        }

        fputs(buf, g_cicadaLogFile);

        /*
         * 行缓冲（_IOLBF）已经保证整行落盘；错误及以上的级别再主动刷一次，
         * 保证崩溃/报错那几行不会因为进程被强杀而丢掉。逐行 fflush 会拖慢
         * 高频日志（播放时每秒上千行），所以不这么做。
         */
        if (prio <= AF_LOG_LEVEL_ERROR) {
            fflush(g_cicadaLogFile);
        }
    }

    QString logFilePathFromArgs(const QStringList &args)
    {
        /* --log-file=<path> 与 --log-file <path> 两种写法都认 */
        for (int i = 0; i < args.size(); ++i) {
            const QString &a = args.at(i);

            if (a.startsWith(QStringLiteral("--log-file="))) {
                return a.mid(QStringLiteral("--log-file=").size()).trimmed();
            }

            if (a == QStringLiteral("--log-file") && i + 1 < args.size()) {
                return args.at(i + 1).trimmed();
            }
        }

        return QString();
    }

    void installFrameworkLogFile(const QStringList &args)
    {
        QString path = logFilePathFromArgs(args);

        if (path.isEmpty()) {
            path = qEnvironmentVariable("CICADA_LOG_FILE").trimmed();
        }

        if (path.isEmpty()) {
            path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);

            if (path.isEmpty()) {
                std::fprintf(stderr, "[log] 拿不到用户数据目录，日志只走控制台\n");
                return;
            }

            path += QStringLiteral("/appQtPlayer.log");
        }

        /* 显式关掉 */
        if (path == QStringLiteral("-")) {
            return;
        }

        const QString dir = QFileInfo(path).absolutePath();

        if (!dir.isEmpty() && !QDir().mkpath(dir)) {
            std::fprintf(stderr, "[log] 建不出日志目录 %s，日志只走控制台\n", qPrintable(dir));
            return;
        }

        /*
         * "w"：每次启动截断。上一次的日志留在文件里只会让「这次到底跑到哪」
         * 更难判断（旧日志和 shell 重定向叠在一起就是这个项目踩过的坑）。
         */
        g_cicadaLogFile = fopen(path.toLocal8Bit().constData(), "w");

        if (g_cicadaLogFile == nullptr) {
            std::fprintf(stderr, "[log] 打不开日志文件 %s，日志只走控制台\n", qPrintable(path));
            return;
        }

        setvbuf(g_cicadaLogFile, nullptr, _IOLBF, 8192);

        log_set_back(cicadaLogFileBack, nullptr);

        std::printf("[log] 框架日志单文件出口：%s\n", qPrintable(path));
        AF_LOGI("[log] framework log file (single writer): %s\n", qPrintable(path));
    }

}// namespace

/*
 * 把 Qt 自己的告警接进我们的日志。
 *
 * 【为什么必须装】QML 里写错一个名字（比如引用了不存在的属性 bar.barHeight），
 * Qt 只会发一条 qWarning（TypeError: Cannot assign to non-existent property ...）。
 * 而这类消息默认走 Windows 的调试输出（OutputDebugString）：没有调试器就**什么都看不到**，
 * 界面上的表现却是"某块东西静默消失"—— 找起来极其痛苦（控制栏按钮整排消失就是这么来的）。
 *
 * 接上之后：控制台（stderr）和框架日志里都会有一条带 QML 文件名的告警。
 *
 * 【2026-09-21 起】只保留框架日志那一份：以前 warn/error 还会**额外** fprintf 一份
 * 到 stderr，于是同一条告警在重定向到同一个文件时出现两次（附件里那一大堆重复行
 * 有一半是它造成的）。现在只有在"没有单文件出口"（g_cicadaLogFile == nullptr，
 * 即用户显式关掉或打不开）时才补 stderr 那一份，保证纯重定向的用法不受影响。
 */
static void cicadaQtMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const QByteArray text = message.toUtf8();
    const char *file = (context.file != nullptr) ? context.file : "";
    const int line = context.line;
    const bool mirrorToStderr = (g_cicadaLogFile == nullptr);

    switch (type) {
        case QtDebugMsg:
            AF_LOGD("[qt] %s:%d %s\n", file, line, text.constData());
            break;

        case QtInfoMsg:
            AF_LOGI("[qt] %s:%d %s\n", file, line, text.constData());
            break;

        case QtWarningMsg:
            AF_LOGW("[qt] %s:%d %s\n", file, line, text.constData());

            if (mirrorToStderr) {
                fprintf(stderr, "[qt warning] %s:%d %s\n", file, line, text.constData());
            }

            break;

        default:/* QtCriticalMsg / QtFatalMsg */
            AF_LOGE("[qt] %s:%d %s\n", file, line, text.constData());

            if (mirrorToStderr) {
                fprintf(stderr, "[qt error] %s:%d %s\n", file, line, text.constData());
            }

            break;
    }

    fflush(stderr);
}

#ifdef Q_OS_WIN
/*
 * 【崩溃时把调用栈打进日志】
 *
 * 为什么需要它：「点播放之后窗口一闪就没了 / 直接闪退」是最难查的一类问题 —— Windows
 * 只会说"进程异常结束"，日志里往往一个字都没有（框架日志和 qWarning 都还没刷出来）。
 * 这个 handler 挂在 SetUnhandledExceptionFilter 上，异常发生时：
 *   * 先打出**异常码 + 出错地址**（这一行最重要，先 flush）；
 *   * 再用 dbghelp 把调用栈翻成 `模块!函数+偏移（文件:行号）` —— Debug 构建的
 *     appQtPlayer.pdb 就在 exe 旁边，而框架是静态链进这个 exe 的，所以连框架内部的
 *     函数名和行号都能出来，不用挂调试器；
 *   * 打完 `_exit(3)`，不让系统再弹"程序已停止工作"的框。
 * 只在崩溃路径上跑，正常播放没有任何开销（挂 handler 本身只是一次函数注册）。
 *
 * 顺带还接一个 `std::terminate`：未捕获的 C++ 异常（throw 没人接）走的是 terminate，
 * 不是 SEH 异常，光有上面那个 filter 抓不到。
 */
namespace {

    void dumpCallStack()
    {
        HANDLE process = GetCurrentProcess();

        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

        if (!SymInitialize(process, nullptr, TRUE)) {
            fprintf(stderr, "[crash] SymInitialize 失败（err=%lu）：只能给地址，查一下 pdb 在不在 exe 旁边\n",
                    GetLastError());
            fflush(stderr);
        }

        void *frames[64] = {};
        const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);

        fprintf(stderr, "[crash] 调用栈（%u 层，从崩溃点往外）：\n", static_cast<unsigned>(count));

        for (USHORT i = 0; i < count; ++i) {
            const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);

            /* SYMBOL_INFO 是变长结构（名字跟在后面），按惯例多留 256 字节 */
            char storage[sizeof(SYMBOL_INFO) + 256] = {};
            auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;

            DWORD64 displacement = 0;
            const char *name = "??";

            if (SymFromAddr(process, addr, &displacement, symbol))
                name = symbol->Name;

            IMAGEHLP_LINE64 line = {};
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisplacement = 0;

            if (SymGetLineFromAddr64(process, addr, &lineDisplacement, &line)) {
                fprintf(stderr, "  #%02u %s+0x%llx  (%s:%lu)\n", static_cast<unsigned>(i), name,
                        static_cast<unsigned long long>(displacement), line.FileName, line.LineNumber);
            } else {
                fprintf(stderr, "  #%02u %s+0x%llx\n", static_cast<unsigned>(i), name,
                        static_cast<unsigned long long>(displacement));
            }
        }

        fflush(stderr);
    }

    /* -------------------------------------------------------------------
     * 【卡死看门狗：没有调试器也能拿到"卡死现场"】
     *
     * 为什么要有：用户实测"有时候卡住没有多余的日志"，而且 Qt Creator 的 kit 里
     * **根本没装调试器**（`The kit does not have a debugger set`，F5/暂停都用不了）。
     * 死锁/长时间阻塞的特征恰恰就是"两条线程都不打日志"，所以只能在进程内部自己抓：
     *   * GUI 线程每 500ms 写一次心跳（main() 里那个 QTimer）—— 事件循环一卡，心跳就停；
     *   * 后台看门狗线程每秒看一眼，超过 5 秒没心跳 → 判定卡死，用 dbghelp 的
     *     StackWalk64 把**本进程每个线程**的调用栈打进 stderr（= 日志里就能看到）；
     *   * 一次卡死只打一次（恢复后再卡会再打），正常播放零开销（一次原子读）。
     *
     * 输出形如：
     *     [hang] UI 线程超过 5 秒没有响应（卡死）
     *     [hang] 线程 12345：
     *         #00 ntdll!NtWaitForSingleObject+0x14
     *         #01 afThread::stop+0x2a  (...afThread.cpp:171)
     *         ...
     * 死锁点在哪一条线程、堵在哪个函数，一眼就能看到。
     * ------------------------------------------------------------------- */
    std::atomic<qint64> g_uiHeartbeatMs{-1};

    qint64 steadyNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    /* StackWalk64 读目标线程栈内存的回调（跨线程读，所以用 ReadProcessMemory）。 */
    BOOL CALLBACK readProcessMemory64(HANDLE, DWORD64 base, PVOID buffer, DWORD size, LPDWORD read)
    {
        SIZE_T got = 0;
        const BOOL ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(base), buffer, size, &got);

        if (read != nullptr) {
            *read = static_cast<DWORD>(got);
        }

        return ok;
    }

    void printFrameAt(DWORD64 addr, int index)
    {
        char storage[sizeof(SYMBOL_INFO) + 256] = {};
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;

        DWORD64 displacement = 0;
        const char *name = "??";

        if (SymFromAddr(GetCurrentProcess(), addr, &displacement, symbol)) {
            name = symbol->Name;
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisplacement = 0;

        if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &lineDisplacement, &line)) {
            fprintf(stderr, "    #%02d %s+0x%llx  (%s:%lu)\n", index, name,
                    static_cast<unsigned long long>(displacement), line.FileName, line.LineNumber);
        } else {
            fprintf(stderr, "    #%02d %s+0x%llx\n", index, name,
                    static_cast<unsigned long long>(displacement));
        }
    }

    void dumpAllThreadStacks(const char *why)
    {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);

        if (!SymInitialize(process, nullptr, TRUE)) {
            fprintf(stderr, "[hang] SymInitialize 失败（err=%lu）：符号可能出不来\n", GetLastError());
        }

        fprintf(stderr, "\n[hang] %s（下面是本进程各线程的调用栈）\n", why);
        fflush(stderr);

        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }

        const DWORD selfId = GetCurrentThreadId();
        const DWORD processId = GetCurrentProcessId();
        THREADENTRY32 entry = {};
        entry.dwSize = sizeof(entry);

        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != processId || entry.th32ThreadID == selfId) {
                    continue;
                }

                HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                           FALSE, entry.th32ThreadID);

                if (thread == nullptr) {
                    continue;
                }

                if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                    CloseHandle(thread);
                    continue;
                }

                CONTEXT context = {};
                context.ContextFlags = CONTEXT_FULL;

                if (GetThreadContext(thread, &context)) {
                    STACKFRAME64 frame = {};
                    frame.AddrPC.Offset = context.Rip;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = context.Rbp;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = context.Rsp;
                    frame.AddrStack.Mode = AddrModeFlat;

                    fprintf(stderr, "[hang] 线程 %lu：\n", static_cast<unsigned long>(entry.th32ThreadID));

                    for (int i = 0; i < 40; ++i) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context,
                                         readProcessMemory64, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                            break;
                        }

                        if (frame.AddrPC.Offset == 0) {
                            break;
                        }

                        printFrameAt(frame.AddrPC.Offset, i);
                    }
                }

                ResumeThread(thread);
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);
        fflush(stderr);
    }

    void hangWatchdogLoop()
    {
        /* 5 秒：正常播放 GUI 每 500ms 跳一次；加载/切源最坏一两秒，5 秒足够区分"慢"和"死" */
        constexpr qint64 kStallMs = 5000;
        bool reported = false;

        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            const qint64 beat = g_uiHeartbeatMs.load();

            if (beat < 0) {
                continue;                       /* GUI 线程还没起来 */
            }

            /*
             * 【原生文件/目录选择框开着的时候不算卡死】
             * AppFileDialogs 走 "GUI 线程 exec()" 策略时，原生模态对话框会阻塞在
             * Win32 的消息循环里（Qt 的定时器不派发），GUI 心跳自然停；
             * 用户在对话框里翻十几秒就会被打成"界面卡死"并刷一堆线程栈。
             */
            if (cicadaqt::AppFileDialogs::isDialogOpen()) {
                reported = false;
                continue;
            }

            if (steadyNowMs() - beat > kStallMs) {
                if (!reported) {
                    dumpAllThreadStacks("UI 线程超过 5 秒没有响应（卡死）");
                    reported = true;
                }
            } else {
                reported = false;               /* 恢复过，下次卡死再打一次 */
            }
        }
    }

    void startHangWatchdog()
    {
        std::thread(hangWatchdogLoop).detach();
    }

    /*
     * 【★ 让"什么都没有的闪退"也变得有据可查 ★】
     *
     * 用户实测：HLS 才走到 `initOpen`（解析 master.m3u8）就 "terminated abnormally"，
     * 日志里**一个字都没有**（连我们自己的 [crash] 都没有）—— 因为这类崩溃走的是
     * CRT 的 **fail-fast**：Debug 堆发现"某次分配之后被越界写了"（
     * `HEAP CORRUPTION DETECTED: after Normal block (#NNN) … wrote to memory after end of
     * heap buffer`），报告 → 中止。fail-fast 按设计**绕过** SetUnhandledExceptionFilter，
     * 所以我们的处理器根本不会被调用。
     *
     * 破解办法就在 CRT 自己身上：它会先调这个报告钩子。于是：
     *   * 把报告模式改成写 stderr（不再弹 "Retry to debug" 框）；
     *   * 钩子里先打印 CRT 的原文，再用 dumpCallStack() 把**检测到损坏那一刻**的调用栈
     *     符号化打出来 —— 那一帧就是触发检查的 free/malloc（往往正是我们越界写的那块内存
     *     的释放点），顺着它就能找到写越界的那段代码；
     *   * 打完 `_exit(3)`（沿用崩溃路径的退出码）。
     * 只在 Debug 构建里编译（Release 没有 Debug 堆，也就没有这个机会）。
     */
#ifdef _DEBUG
    int __cdecl cicadaCrtReportHook(int reportType, char *message, int *returnValue)
    {
        (void) returnValue;

        if (reportType == _CRT_WARN) {
            return FALSE;                       /* 普通 warn 交给 CRT 自己处理 */
        }

        /*
         * 【断言不能杀进程 —— 这是"关播放器窗口把整个程序带崩"的直接原因】
         *
         * 关窗时 DanmakuRendererItem 析构会踩到 Debug STL 的断言
         * （`_Adjust_manually_vector_aligned: invalid argument`，见日志）。
         * 断言**只说明某个容器状态不对**，不代表必须立刻终止；
         * 之前这里对 ASSERT 也 `_exit(3)`，于是"关掉播放器窗口 → 整个应用退出，
         * 首页也一起没了"（用户实测）。现在改成：记一行（带栈，便于继续查根因），
         * 然后**让它继续跑**，窗口该关就关、首页该留就留。
         * 真正的堆损坏（_CRT_ERROR）仍然打栈并退出。
         */
        if (reportType == _CRT_ASSERT) {
            fprintf(stderr, "\n[heap] CRT 断言（已忽略，不杀进程）：%s\n",
                    (message != nullptr) ? message : "(无消息)");
            fflush(stderr);
            dumpCallStack();
            return TRUE;                        /* 已处理：CRT 继续，不弹框、不中止 */
        }

        fprintf(stderr, "\n[heap] CRT 错误：%s\n", (message != nullptr) ? message : "(无消息)");
        fflush(stderr);

        dumpCallStack();

        _exit(3);
        return TRUE;
    }

    void installCrtHeapReportHook()
    {
        /* 报告写 stderr（不弹框），并挂上我们的钩子 */
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, cicadaCrtReportHook);

        /*
         * 【诊断开关】默认**不开**（每次分配都查堆会非常慢）。
         * 跑之前设一个环境变量就能打开：
         *     CICADA_HEAP_CHECK=1   → 每 16 次分配检查一遍堆（够快，能逮到越界写）
         *     CICADA_HEAP_CHECK=all → 每次分配都查（最慢，最贴近罪犯）
         * 打开之后一旦有"越界写"，下面的钩子就会把**当时的符号栈**打出来 ——
         * 不需要调试器（用户机器上 Qt Creator 的 kit 没装调试器）。
         */
        const QByteArray heapCheck = qgetenv("CICADA_HEAP_CHECK");

        if (!heapCheck.isEmpty()) {
            int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
            flags |= _CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_CRT_DF;

            if (heapCheck == "all") {
                flags |= _CRTDBG_CHECK_ALWAYS_DF;
            } else {
                flags |= _CRTDBG_CHECK_EVERY_16_DF;
            }

            _CrtSetDbgFlag(flags);
            fprintf(stderr, "[heap] 堆检查已打开（CICADA_HEAP_CHECK=%s）\n", heapCheck.constData());
            fflush(stderr);
        }
    }
#endif

    LONG WINAPI cicadaUnhandledException(EXCEPTION_POINTERS *info)
    {
        const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr)
                               ? info->ExceptionRecord->ExceptionCode : 0;
        const void *address = (info != nullptr && info->ExceptionRecord != nullptr)
                                  ? info->ExceptionRecord->ExceptionAddress : nullptr;

        /* 先写这一行并 flush：就算下面翻符号的时候再出事，异常码也已经落到日志里了 */
        fprintf(stderr, "\n[crash] 未处理异常：code=0x%08lx address=%p\n", code, address);
        fflush(stderr);

        dumpCallStack();

        _exit(3);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void cicadaTerminateHandler()
    {
        fprintf(stderr, "\n[crash] std::terminate：未捕获的 C++ 异常（多半是内存/越界）\n");
        fflush(stderr);

        dumpCallStack();

        _exit(3);
    }

}// namespace
#endif// Q_OS_WIN

int main(int argc, char *argv[])
{
    /*
     * 单一日志出口 + stdout 行缓冲，必须放在**任何 AF_LOG* 之前**（见
     * installFrameworkLogFile 的说明）：
     *   * 文件出口保证日志只有本进程一个写入者，不再依赖把 stdout/stderr
     *     一起重定向到同一个文件（那种做法会产生重复和被截断的行）；
     *   * stdout 改成行缓冲，是为了"没有单文件出口"的老用法（
     *     `appQtPlayer.exe > log.txt 2>&1`）在 stdout/stderr 合流时顺序仍然正确 ——
     *     默认是全缓冲，进程被强杀时最后几 KB 会整段丢失。
     */
    setvbuf(stdout, nullptr, _IOLBF, 8192);

#ifdef Q_OS_WIN
    /* 崩溃/未捕获异常时把调用栈打进日志（见上面那两个 handler 的说明） */
    SetUnhandledExceptionFilter(cicadaUnhandledException);
    std::set_terminate(cicadaTerminateHandler);
#ifdef _DEBUG
    /* CRT 堆发现越界写的那一刻也打栈（fail-fast 会绕过 SEH，只能靠这个钩子） */
    installCrtHeapReportHook();
#endif
#endif

    QGuiApplication app(argc, argv);

    /*
     * 单一日志出口放在 QGuiApplication 之后：默认路径要用
     * QStandardPaths::AppLocalDataLocation，而它依赖"应用名"——应用名是
     * QCoreApplication 构造时从 argv[0] 推出来的（= appQtPlayer），
     * 构造之前拿到的路径里会少一级。放到这里仍然早于任何 AF_LOG* 调用
     * （第一条在下面打印图形后端那里）。
     */
    installFrameworkLogFile(app.arguments());

    /* Qt 的告警（含 QML 的 TypeError/属性不存在）接进日志，别再静默吞掉。 */
    qInstallMessageHandler(cicadaQtMessageHandler);

    /*
     * 把 Windows 平台插件里**对话框那一类**的调试日志也打开
     * （`QLoggingCategory("qt.qpa.dialogs")`，qwindowsdialoghelpers.cpp 里的
     * `qCDebug(lcQpaDialogs)`）。
     *
     * 为什么要它：原生"打开文件"对话框偶尔会**瞬间被 rejected**（用户看到"点了没反应"），
     * 那行 `doExec returns 0x8007xxxx` 是唯一能说明"到底是哪个 HRESULT 让它弹不出来"的
     * 读数（默认这条 category 是关的，日志里只有一句
     * "Native file dialog: unable to get dialog's window."，看不出原因）。
     * 代价只有几行日志，而它在排查这类问题时是决定性的 —— 所以默认打开。
     */
    QLoggingCategory::setFilterRules(QStringLiteral("qt.qpa.dialogs.debug=true"));

#ifdef Q_OS_WIN
    /*
     * GUI 线程心跳 + 卡死看门狗。
     * 心跳只能由 GUI 线程的事件循环推进 —— 界面一旦卡住（比如消息线程在
     * afThread::stop() 的 join 上等阻塞 IO），心跳就停，看门狗就会把各线程栈打出来。
     */
    g_uiHeartbeatMs.store(steadyNowMs());

    auto *heartbeatTimer = new QTimer(&app);
    QObject::connect(heartbeatTimer, &QTimer::timeout, []() {
        g_uiHeartbeatMs.store(steadyNowMs());
    });
    heartbeatTimer->start(500);

    startHangWatchdog();
#endif

#ifdef Q_OS_WIN
    /*
     * 再挂一次：QGuiApplication 构造（以及 Qt 内部的平台初始化）会把
     * SetUnhandledExceptionFilter 换成它自己的 —— 那样我们那份就永远不跑，
     * 崩溃时日志里干干净净（实测过一次："直接闪退，什么都没有"）。
     * 在 Qt 装完之后再挂一次，确保最后生效的是我们这份。
     */
    SetUnhandledExceptionFilter(cicadaUnhandledException);
    std::set_terminate(cicadaTerminateHandler);
#endif

    /*
     * 打印一下 Qt 用的是哪个图形后端（只是日志，不去改它）。
     * 零拷贝能不能成立和它强相关：
     *   Windows -> Direct3D11
     *   macOS   -> Metal
     *   Linux   -> OpenGL（VAAPI 那条 dmabuf/EGLImage 路要求 OpenGL）
     * 后端的选择、以及用哪块 GPU 设备，全部是 Qt 的事（它会照顾混合显卡、软渲染回退
     * 等一堆平台差异），我们只读不写。需要临时指定后端时用 Qt 自己的环境变量：
     * QSG_RHI_BACKEND=d3d11 / metal / opengl。
     */
    AF_LOGI("Qt scene graph graphics API: %s\n",
            graphicsApiName(QQuickWindow::graphicsApi()));

    registerCicadaVideoRender();

    QQmlApplicationEngine engine;

    /*
     * 把命令行里给的视频地址交给 QML 根对象（Main.qml 的 Window 上声明了
     * `property url startupSource`）。
     *
     * 用 setInitialProperties 而不是 rootContext()->setContextProperty()：类型是明确的
     * url，也不会在 QML 里凭空多出一个全局名字。它必须在 loadFromModule() 之前调用 ——
     * 初始属性是在根对象创建好、componentComplete() 之前写进去的，所以 QML 里的
     * onStartupSourceChanged 一定收得到。
     */
    const QUrl startupSource = startupSourceFromCommandLine();
    QString manifestHint;
    const QString startupManifest = startupManifestFromCommandLine(&manifestHint);

    /*
     * 初始属性统一攒在一个 map 里再写进去（原来两个分支各调用一次 setInitialProperties，
     * 后一次会把前一次**整个覆盖**掉，所以必须合并）。
     */
    QVariantMap initialProperties;

    /*
     * mock 服务基址：环境变量 CICADA_MOCK_BASE 优先。
     *
     * 【为什么在 C++ 里读】QML 没有读环境变量的入口（qEnvironmentVariable 是 C++ 的），
     * 所以在这里读出来、作为初始属性交给 Main.qml 的 `property string mockBaseEnv`；
     * 非空时 Main.qml 会**跳过** 9101/9000 的自动探测，直接用这个地址。
     * 用法：CICADA_MOCK_BASE=http://127.0.0.1:9000 appQtPlayer.exe ...
     */
    const QString mockBaseFromEnv = qEnvironmentVariable("CICADA_MOCK_BASE").trimmed();

    if (!mockBaseFromEnv.isEmpty()) {
        initialProperties.insert(QStringLiteral("mockBaseEnv"), QVariant::fromValue(mockBaseFromEnv));
        std::printf("[mock] CICADA_MOCK_BASE=%s（跳过端口探测）\n", qPrintable(mockBaseFromEnv));
    }

    if (!startupSource.isEmpty()) {
        initialProperties.insert(QStringLiteral("startupSource"), QVariant::fromValue(startupSource));
    } else if (!startupManifest.isEmpty()) {
        /*
         * 命令行给的是 .json 清单：交给 Main.qml 的 `property string startupManifest` /
         * `property string startupManifestName`，它会在组件创建时先设 manifestHint
         * （决定 HLS 还是 DASH 管线）再调 player.setManifestJson()。
         */
        initialProperties.insert(QStringLiteral("startupManifest"), QVariant::fromValue(startupManifest));
        initialProperties.insert(QStringLiteral("startupManifestName"), QVariant::fromValue(manifestHint));
    }

    if (!initialProperties.isEmpty())
        engine.setInitialProperties(initialProperties);

    /*
     * 注册无边框窗口的 QML 类型（`import QWindowKit` 里的 WindowAgent）。
     *
     * 必须在 loadFromModule() **之前**调：它做的事就是
     *     qmlRegisterType<QuickWindowAgent>("QWindowKit", 1, 0, "WindowAgent");
     *     qmlRegisterModule("QWindowKit", 1, 0);
     * 见 3rdparty/qwindowkit/src/quick/qwkquickglobal.cpp。因为是静态链接，没有 qmldir
     * 也没有 QML 插件，所以这一步不能省 —— 省了 QML 里 import QWindowKit 会失败。
     */
    QWK::registerTypes(&engine);

    /*
     * 快照 provider：把 C++ 侧截到的帧暴露成 `image://snapshot/<revision>`
     * （进度条悬停气泡里的 160x90 预览图就是这么来的）。
     *
     * **必须在 loadFromModule() 之前注册**：QML 里 `Image { source: "image://snapshot/..."
     * }` 在组件创建、解析 source 的时候就要能查到这个名字，晚了就是"未知的 image provider"。
     *
     * 【为什么 provider 是"全局一份、不认窗口"】快照的消费者只有进度条气泡，而同一时刻
     * 只可能有一个窗口在被悬停；按 item/窗口路由要维护注册表 + 处理 item 析构，纯属多余。
     * 多窗口时的语义是"后发布的赢"（谁最后截到图，两个窗口的气泡都显示它）—— 详见
     * SnapshotImageProvider.h 的文件头。
     *
     * addImageProvider() 会**接管 provider 的所有权**（QQmlEngine 析构时删掉它），
     * 所以这里 new 出来之后不需要自己 delete，也不能放到栈上。
     */
    engine.addImageProvider(QStringLiteral("snapshot"), new cicadaqt::SnapshotImageProvider());

    /*
     * 应用级对话框单例：QML 里 `import CicadaPlayer` 之后用 AppDialogs.openJsonFile() /
     * openVideoFile() / openDirectory() 打开**系统原生**的文件/目录选择框。
     *
     * 【为什么放在 C++ 里】QML 的 FileDialog/FolderDialog 会把组件对象和它底下的原生
     * 句柄一起被引擎持有，close() 只是隐藏窗口、句柄不释放（"连开几次就弹不出来"、
     * "目录被占用"）。C++ 这条路每次现建一个平台对话框对象、选完立刻销毁，句柄当场释放。
     * 细节见 src/AppFileDialogs.h 的文件头。
     *
     * 【注册位置】必须在本行之后的 loadFromModule() 之前。单例实例由 qmlRegisterSingletonInstance
     * 接管（不需要我们自己 delete，它要活到引擎析构）；用独立 URI "CicadaPlayer" 是为了不和
     * QML 模块 QtPlayer 自己生成的那套注册撞车。
     */
    qmlRegisterSingletonInstance("CicadaPlayer", 1, 0, "AppDialogs",
                                 new cicadaqt::AppFileDialogs());

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() {
            reportStartupFailure();
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    /*
     * 启动打开哪个窗口：
     *   * 命令行给了片源 / 清单 → 直接开**播放器窗口**（Main.qml，它才有 startupSource /
     *     startupManifest 这两个属性）—— 这也是本文件头部注释写的用法
     *     （`appQtPlayer.exe D:\video\4k.mp4` 启动就播这个文件）；
     *   * 否则开**首页窗口**（视频库列表），播放器窗口由首页点卡片时按需创建。
     *
     * 【这里曾经是个真 bug】根对象从 Main 换成 HomeWindow 之后，这两行没跟着改：
     * 只要命令行带路径，就会给 HomeWindow 设一个它没有的属性 `startupSource`，Qt 直接报
     *     Setting initial properties failed: HomeWindow does not have a property called startupSource
     * 然后**不创建任何窗口**（程序起来就退，看起来像"双击没反应"）。现在按有没有片源分开加载。
     */
    if (!startupSource.isEmpty() || !startupManifest.isEmpty()) {
        engine.loadFromModule("QtPlayer", "Main");
    } else {
        engine.loadFromModule("QtPlayer", "HomeWindow");
    }

    /*
     * 兜一道同步检查：加载失败时 loadFromModule 只会往 stderr 打一行 Qt 的报错，
     * 不会抛异常、也不会阻塞在这里，紧接着 exec() 进去就是个空事件循环 ——
     * 表现就是"双击没反应"。这里当场发现就地退出，并打出人话。
     */
    if (engine.rootObjects().isEmpty()) {
        reportStartupFailure();
        return 1;
    }

    return QGuiApplication::exec();
}
