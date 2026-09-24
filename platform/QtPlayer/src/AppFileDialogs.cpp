//
// AppFileDialogs.cpp
//
// 实现见头文件里的说明（为什么放在 C++ 里、为什么用平台对话框抽象）。
//
// 关键点只有一个：**对话框对象是局部生命周期**。
//   * 每次调用 `createPlatformDialogHelper()` 造一个；
//   * 用户选完之后（accept/reject 信号）读结果、退出嵌套事件循环；
//   * 对象立刻 deleteLater() —— 原生 IFileDialog / COM 句柄随析构释放。
// 这样就不存在"QML 引擎一直拿着一个已经关掉的对话框"这种状态。
//
#include "AppFileDialogs.h"

#include <utils/frame_work_log.h>

#include <atomic>
#include <memory>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#ifdef Q_OS_WIN
/* 工作线程要自己初始化 COM（IFileDialog 是 STA 对象，公寓必须和创建它的线程一致）。 */
#include <QtCore/qt_windows.h>
#include <objbase.h>
#endif

#ifdef CICADA_QT_HAVE_PLATFORM_FILE_DIALOG
/* QtGui 私有头（CMakeLists 里链了 Qt6::GuiPrivate 才有）：
 *   qplatformtheme.h        —— QPlatformTheme::createPlatformDialogHelper()
 *   qplatformdialoghelper.h —— QPlatformFileDialogHelper / QFileDialogOptions
 *   qguiapplication_p.h     —— QGuiApplicationPrivate::platformTheme()
 * 这三样正是 Qt 自己（QtQuick.Dialogs 的 FileDialog）内部用的同一套东西。 */
#include <QtGui/private/qguiapplication_p.h>
#include <QtGui/qpa/qplatformdialoghelper.h>
#include <QtGui/qpa/qplatformtheme.h>
#endif

namespace cicadaqt {

    namespace {
        /* 原生对话框是否正在显示（看门狗用，见 isDialogOpen 的说明）。 */
        std::atomic<bool> g_dialogOpen{false};
    }

    bool AppFileDialogs::isDialogOpen()
    {
        return g_dialogOpen.load();
    }

    AppFileDialogs::AppFileDialogs(QObject *parent)
        : QObject(parent)
    {
    }

    bool AppFileDialogs::available() const
    {
#ifdef CICADA_QT_HAVE_PLATFORM_FILE_DIALOG
        return true;
#else
        return false;
#endif
    }

    void AppFileDialogs::setLastError(const QString &error)
    {
        if (m_lastError == error) {
            return;
        }

        m_lastError = error;
        emit lastErrorChanged();
    }

    QString AppFileDialogs::openVideoFile(const QString &startPath)
    {
        return runDialog(false, tr("选择要播放的视频文件"), startPath,
                         QStringList{ tr("视频文件 (*.mp4 *.mkv *.mov *.flv *.ts *.m2ts *.avi *.webm *.wmv *.mpg *.mpeg)"),
                                      tr("所有文件 (*)") });
    }

    QString AppFileDialogs::openJsonFile(const QString &startPath)
    {
        return runDialog(false, tr("选择清单 JSON 文件"), startPath,
                         QStringList{ tr("清单 JSON (*.json)"), tr("所有文件 (*)") });
    }

    QString AppFileDialogs::openDirectory(const QString &startPath)
    {
        return runDialog(true, tr("选择视频文件夹"), startPath, QStringList{});
    }

#ifdef CICADA_QT_HAVE_PLATFORM_FILE_DIALOG

    namespace {

        /*
         * 跑一小段事件循环（而不是 sleep/msleep）：
         * 重试之间要让平台那边的事件（窗口激活、定时器、Qt 的两段式对话框状态）
         * 有机会真正跑完，同时又不能让界面卡住。
         */
        void pumpEvents(int ms)
        {
            QEventLoop loop;
            QTimer::singleShot(ms, &loop, &QEventLoop::quit);
            loop.exec();
        }

        /*
         * 把窗口弄到前台并**等它真的成为焦点窗口**。
         *
         * 【为什么必须等，而不只是 requestActivate()】上一轮的 QML 版本之所以能
         * 偶尔成功，是因为 QML 的 FileDialog 是"两段式可见性"：open() 只记下意图，
         * 原生对话框要到**下一轮事件循环**才建 —— 中间那一轮正好让 requestActivate()
         * 的激活生效。C++ 里直接 show() 没有这个间隔，激活还没落地就去开原生模态框，
         * Windows 的前台锁会直接把它拒掉（日志里的
         * `Native file dialog: unable to get dialog's window.` + 立刻 rejected，
         * 用户看到的就是"点了没反应"）。
         * 所以这里 requestActivate() 之后**等焦点窗口真的变成它**（最多 timeoutMs）。
         */
        bool bringToFront(QWindow *w, int timeoutMs = 500)
        {
            if (w == nullptr) {
                return false;
            }

            w->raise();
            w->requestActivate();

            QElapsedTimer elapsed;
            elapsed.start();

            while (QGuiApplication::focusWindow() != w && elapsed.elapsed() < timeoutMs) {
                pumpEvents(15);
            }

            return QGuiApplication::focusWindow() == w;
        }

        /* 挑一个最适合当父窗口的窗口：焦点窗口 > 第一个可见的顶层窗口。 */
        QWindow *pickParentWindow(const QWindow *preferred)
        {
            QWindow *parent = const_cast<QWindow *>(preferred);

            if (parent == nullptr) {
                parent = QGuiApplication::focusWindow();
            }

            if (parent != nullptr && parent->isVisible()) {
                return parent;
            }

            const QList<QWindow *> windows = QGuiApplication::topLevelWindows();

            for (QWindow *w : windows) {
                if (w != nullptr && w->isVisible()) {
                    return w;
                }
            }

            return parent;
        }

    }// anonymous namespace

    namespace {

        /*
         * COM 公寓的 RAII 守卫。
         *
         * 【为什么必须 RAII】工作线程里 CoInitializeEx 之后有好几条提前 return 的路
         * （helper 造不出来、show 失败……）。手工在每个 return 前补 CoUninitialize 迟早
         * 会漏一处，而漏掉就是这条线程的公寓不释放（COM 会一直记着这个 STA，进程级
         * 资源跟着涨；反复开关对话框就会累积）。用析构函数保证"进过这个函数就一定配对"。
         */
        struct ComApartment {
#ifdef Q_OS_WIN
            bool initialized = false;

            ComApartment()
            {
                /* 和工作线程的公寓绑定（IFileDialog 是 STA 对象） */
                initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            }

            ~ComApartment()
            {
                if (initialized) {
                    CoUninitialize();
                }
            }
#else
            ~ComApartment() = default;
#endif
        };

        /* 一次对话框请求/结果（工作线程和 GUI 线程之间传递用）。 */
        struct DialogRequest {
            bool selectDirectory = false;
            QString title;
            QString startPath;
            QStringList nameFilters;
        };

        struct DialogOutcome {
            QString selected;       /* 选中的路径（空 = 没选） */
            bool accepted = false;  /* 用户点了确定并且拿到了路径 */
            bool shown = false;     /* 平台真的把对话框弹出来了 */
            bool cancelled = false; /* 是用户取消的（不是平台拒绝） */
            qint64 elapsedMs = 0;
        };

        /* 造一个配置好的对话框 helper。**在哪个线程调用，对象就属于哪个线程**。 */
        QPlatformFileDialogHelper *buildHelper(QPlatformTheme *theme, const DialogRequest &req)
        {
            auto *helper = qobject_cast<QPlatformFileDialogHelper *>(
                    theme->createPlatformDialogHelper(QPlatformTheme::FileDialog));

            if (helper == nullptr) {
                return nullptr;
            }

            QSharedPointer<QFileDialogOptions> options = QFileDialogOptions::create();
            options->setWindowTitle(req.title);
            options->setFileMode(req.selectDirectory ? QFileDialogOptions::DirectoryOnly
                                                     : QFileDialogOptions::ExistingFile);
            options->setAcceptMode(QFileDialogOptions::AcceptOpen);
            options->setLabelText(QFileDialogOptions::Accept, AppFileDialogs::tr("打开"));
            options->setLabelText(QFileDialogOptions::Reject, AppFileDialogs::tr("取消"));

            if (!req.nameFilters.isEmpty()) {
                options->setNameFilters(req.nameFilters);
            }

            helper->setOptions(options);

            if (!req.startPath.trimmed().isEmpty()) {
                QFileInfo info(req.startPath.trimmed());
                helper->setDirectory(QUrl::fromLocalFile(info.isDir() ? info.absoluteFilePath()
                                                                      : info.absolutePath()));
            }

            return helper;
        }

        /* 从 helper 里取结果（accept/reject 两条路都可能走到这里）。 */
        DialogOutcome readOutcome(QPlatformFileDialogHelper *helper, bool shown, qint64 elapsedMs)
        {
            DialogOutcome outcome;
            outcome.shown = shown;
            outcome.elapsedMs = elapsedMs;

            const QList<QUrl> files = helper->selectedFiles();

            if (!files.isEmpty()) {
                outcome.accepted = true;
                outcome.selected = files.first().toLocalFile();

                if (outcome.selected.isEmpty()) {
                    /* 非本地路径（罕见）：退回 URL 字符串，调用方自己判断。 */
                    outcome.selected = files.first().toString();
                }
            }

            return outcome;
        }

        /*
         * 【策略 A：在工作线程里跑原生对话框】
         *
         * 这是本轮的关键修复。为什么必须这样：
         *
         *  * `IFileDialog` 是 COM 对象，而 **Show() 必须和创建它的线程在同一个公寓里**。
         *    Qt 的两条路分别是：
         *      - `show()` + 调用方自己跑事件循环 → Qt 用一个 0ms 定时器把对话框丢到
         *        **它自己的工作线程**去跑 Show()，而对象是在调用线程（GUI）创建的 →
         *        跨公寓使用 → 实测 4ms 内被 rejected（日志里那条
         *        `Native file dialog: unable to get dialog's window.`），用户看到"点了没反应"；
         *      - `show()` + `exec()` → 在调用线程里跑 Show() ✓ 能弹出来，但**阻塞调用线程**：
         *        GUI 线程一堵，我们的视频帧回调（它是 queued 到 GUI 线程的 update()）
         *        就停了 → 画面冻住、窗口被 Windows 标成"(未响应)"、鼠标指针也不刷新。
         *
         *  所以这里自己开一个线程：**helper 的创建和 Show() 都在这条线程上**（同一个公寓 ✓），
         *  而 GUI 线程只是跑一个嵌套事件循环等它结束（视频、QML、定时器全都照常）。
         *
         * 线程里要自己初始化 COM（Windows 的 STA），并用 sendPostedEvents 把 Qt 那边
         * 用 deleteLater 排队的原生对象（`m_nativeDialog`、helper）在这一轮就销毁掉 ——
         * 不然这条线程没有事件循环，那些对象（和它们持有的原生句柄）会一直留着。
         */
        DialogOutcome runOnWorkerThread(QPlatformTheme *theme, const DialogRequest &req,
                                        QWindow *parent, int attempt)
        {
            DialogOutcome outcome;

            /* 任何提前 return 都会把这个线程的 COM 公寓正确释放（见 ComApartment 的说明） */
            ComApartment com;

            std::unique_ptr<QPlatformFileDialogHelper> helper(buildHelper(theme, req));

            if (helper == nullptr) {
                outcome.cancelled = false;
                return outcome;
            }

            QElapsedTimer elapsed;
            elapsed.start();

            g_dialogOpen.store(true);
            const Qt::WindowModality modality = (parent != nullptr) ? Qt::WindowModal
                                                                    : Qt::ApplicationModal;
            const bool shown = helper->show(Qt::WindowFlags(), modality, parent);

            AF_LOGI("AppFileDialogs: 打开原生对话框 “%s” 第 %d 次（策略=工作线程exec/%s "
                    "parent=%p hwnd=%llu shown=%d）\n",
                    req.title.toUtf8().constData(), attempt + 1,
                    parent != nullptr ? "worker" : "no-parent",
                    static_cast<void *>(parent),
                    parent != nullptr ? (unsigned long long) parent->winId() : 0ULL, (int) shown);

            if (shown) {
                /*
                 * 注意：这里**不能**先跑事件循环。`show()` 会起一个 0ms 定时器，
                 * 一旦被派发，Qt 就把对话框丢到它自己的工作线程去了（就又回到
                 * 跨公寓使用那条死路）。`exec()` 会先把这个定时器停掉，
                 * 然后在**当前线程**里跑 `IFileDialog::Show()` 的模态循环。
                 */
                helper->exec();
            }

            outcome = readOutcome(helper.get(), shown, elapsed.elapsed());
            g_dialogOpen.store(false);

            /* 用户取消：活得够久说明人真的看到它了（700ms 和 QML 那版同一个判据） */
            if (!outcome.accepted) {
                outcome.cancelled = shown && outcome.elapsedMs > 700;
            }

            if (shown) {
                helper->hide();
            }

            /*
             * 【资源的彻底释放】helper 和它内部那个原生对话框对象都是这条线程的，
             * 直接析构（此刻已经不在它自己的信号发射里了：exec() 早就返回）；
             * 之后再把 deleteLater 排的那些（Qt 内部对 m_nativeDialog 用的就是
             * deleteLater）用 sendPostedEvents 立刻派发掉 —— 这条线程没有事件循环，
             * 不主动派发它们就会一直挂到进程结束（原生句柄/COM 对象跟着泄漏）。
             * 后面还会再派发一次：hide()/析构过程中新排进来的 DeferredDelete 也一起清干净。
             */
            helper.reset();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

            /*
             * 这两行是给"句柄到底有没有释放"留证据的：helper 和它内部的原生对话框
             * （IFileDialog/COM 对象、文件系统监视器）都是这条线程的局部对象，
             * 上面两句话之后它们都已经析构 —— 句柄不会像 QML 那样被引擎留到进程结束。
             */
            AF_LOGI("AppFileDialogs: 对话框对象已销毁（原生句柄已释放，worker 线程）\n");

            return outcome;
        }

    }// anonymous namespace

    QString AppFileDialogs::runDialog(bool selectDirectory, const QString &title,
                                      const QString &startPath, const QStringList &nameFilters)
    {
        setLastError(QString());

        if (m_busy) {
            /* 上一个对话框还开着（重复点击 / 快捷键和点击同时到）。 */
            AF_LOGW("AppFileDialogs: 已经有一个原生对话框在显示，忽略这一次 (%s)\n",
                    title.toUtf8().constData());
            return QString();
        }

        QPlatformTheme *theme = QGuiApplicationPrivate::platformTheme();

        if (theme == nullptr) {
            setLastError(tr("拿不到平台主题，无法创建原生对话框"));
            AF_LOGE("AppFileDialogs: platformTheme() is null\n");
            return QString();
        }

        DialogRequest req;
        req.selectDirectory = selectDirectory;
        req.title = title;
        req.nameFilters = nameFilters;
        req.startPath = startPath.trimmed().isEmpty() ? m_lastDir : startPath.trimmed();

        /*
         * 父窗口和"把窗口弄到前台"都必须在 **GUI 线程**上做（会跑事件循环等焦点，
         * 也会碰 QWindow）。做完再把 HWND 交给工作线程去当 owner —— Windows 的
         * 窗口句柄本身不绑定线程，跨线程当 owner 是允许的。
         */
        QWindow *parent = pickParentWindow(nullptr);
        /* 预热一次 winId：让 handleOf() 在工作线程里只是读一个已有句柄 */
        if (parent != nullptr) {
            parent->winId();
        }

        bringToFront(parent);

        m_busy = true;

        /*
         * 第 1、2 次尝试走**工作线程**（见 runOnWorkerThread 的说明：不阻塞 GUI 线程，
         * 视频/鼠标/QML 全都照常；同时避免 Qt 那条跨公寓的失败路径）。
         */
        for (int attempt = 0; attempt < 2; ++attempt) {
            DialogOutcome outcome;

            QThread *worker = QThread::create([theme, &req, &outcome, parent, attempt]() {
                outcome = runOnWorkerThread(theme, req, parent, attempt);
            });
            worker->start();

            {
                /*
                 * GUI 线程在这里**保持事件循环**（嵌套 QEventLoop，而不是 sleep/wait）：
                 * 视频帧回调、QML 定时器、场景图重绘、鼠标指针更新全都不受影响 ——
                 * 这正是上一版 `helper->exec()` 做不到的（那会堵死 GUI 线程，
                 * 用户看到"未响应 / 播放窗口出问题 / 鼠标指针不刷新"）。
                 */
                QEventLoop loop;
                QObject::connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
                loop.exec();
            }

            worker->wait();
            delete worker;

            if (outcome.accepted && !outcome.selected.isEmpty()) {
                m_lastDir = QFileInfo(outcome.selected).absolutePath();
                m_busy = false;
                AF_LOGI("AppFileDialogs: 选中 %s（工作线程策略，第 %d 次）\n",
                        outcome.selected.toUtf8().constData(), attempt + 1);
                return outcome.selected;
            }

            if (outcome.cancelled) {
                m_busy = false;
                AF_LOGI("AppFileDialogs: 用户取消（%s，开了 %lld ms）\n",
                        title.toUtf8().constData(), (long long) outcome.elapsedMs);
                return QString();
            }

            setLastError(tr("系统没能弹出对话框（第 %1 次尝试失败，共 3 次）").arg(attempt + 1));
            AF_LOGW("AppFileDialogs: 工作线程策略没弹出来（shown=%d %lld ms），重试 %d/2\n",
                    (int) outcome.shown, (long long) outcome.elapsedMs, attempt + 1);
            pumpEvents(150 * (attempt + 1));
        }

        /*
         * 【策略 B：GUI 线程 exec（最后一次尝试）】
         * 工作线程两次都没弹出来时，用这条**已知一定能弹出来**的路兜底：
         * 它会阻塞 GUI 线程（画面/鼠标那几秒不刷新），但总比"点了没反应"强。
         */
        AF_LOGW("AppFileDialogs: 工作线程策略失败，改用 GUI 线程 exec 兜底（会短暂阻塞界面）\n");

        auto *helper = buildHelper(theme, req);

        if (helper == nullptr) {
            m_busy = false;
            setLastError(tr("当前平台没有可用的原生文件对话框"));
            AF_LOGE("AppFileDialogs: createPlatformDialogHelper(FileDialog) returned null\n");
            return QString();
        }

        QElapsedTimer elapsed;
        elapsed.start();

        g_dialogOpen.store(true);
        const Qt::WindowModality modality = (parent != nullptr) ? Qt::WindowModal
                                                                : Qt::ApplicationModal;
        const bool shown = helper->show(Qt::WindowFlags(), modality, parent);

        AF_LOGI("AppFileDialogs: 打开原生对话框 “%s”（策略=GUI线程exec兜底 parent=%p shown=%d）\n",
                title.toUtf8().constData(), static_cast<void *>(parent), (int) shown);

        if (shown) {
            /* show() 之后**不能**跑事件循环：那个 0ms 定时器一旦派发，对话框就跑到
             * Qt 自己的工作线程去了（跨公寓 → 必失败）。exec() 会停掉它并在本线程跑。 */
            helper->exec();
        }

        const DialogOutcome outcome = readOutcome(helper, shown, elapsed.elapsed());
        g_dialogOpen.store(false);

        if (shown) {
            helper->hide();
        }

        /* 这条（GUI）线程有事件循环，deleteLater 会被正常派发 */
        helper->deleteLater();

        m_busy = false;

        if (outcome.accepted && !outcome.selected.isEmpty()) {
            m_lastDir = QFileInfo(outcome.selected).absolutePath();
            AF_LOGI("AppFileDialogs: 选中 %s（GUI 线程兜底）\n",
                    outcome.selected.toUtf8().constData());
            return outcome.selected;
        }

        AF_LOGI("AppFileDialogs: 用户取消（%s，开了 %lld ms）\n",
                title.toUtf8().constData(), (long long) outcome.elapsedMs);
        return QString();
    }

#else// CICADA_QT_HAVE_PLATFORM_FILE_DIALOG

    /*
     * 没有 QtGui 私有头（老 Qt / 精简安装）时的退化实现：什么都不做，返回空串。
     * QML 侧通过 available() == false 会走回原来的 QML FileDialog 路径，功能不丢。
     */
    QString AppFileDialogs::runDialog(bool selectDirectory, const QString &title,
                                      const QString &startPath, const QStringList &nameFilters)
    {
        (void) selectDirectory;
        (void) startPath;
        (void) nameFilters;

        setLastError(tr("这个 Qt 构建没有原生对话框支持（缺 QtGui 私有头）"));
        AF_LOGW("AppFileDialogs: compiled without platform file dialog support, "
                "QML falls back to QtQuick.Dialogs (%s)\n", title.toUtf8().constData());
        return QString();
    }

#endif// CICADA_QT_HAVE_PLATFORM_FILE_DIALOG

}// namespace cicadaqt
