//
// AppFileDialogs.h
//
// 【为什么要有这个东西】QML 的 FileDialog / FolderDialog 会"占住句柄"：
// 组件对象一旦被 QML 引擎持有，它底层那套原生对话框（Windows 上是
// IFileDialog / COM 对象，外加文件系统监视器）就跟着一起活着，哪怕调用了
// close() 也只是把窗口藏起来。表现出来就是"连开几次之后对话框再也弹不出来"
// 以及"某个盘/目录被占用"。
//
// 治本的做法只有一条：**用完就把对话框对象销毁**，让原生句柄随析构一起释放。
// QML 侧要做到这一点得靠"动态创建 + 在 accepted/rejected 里 destroy() + 隔一拍"
// （见 Main.qml 里那段很长的注释，以及 docs 里的分析.md），绕的还是 Qt 内部那套
// "可见性两段式"状态。放在 **C++ 里调用**就没有这个问题：
//   * 每次调用现建一个平台对话框对象（QPlatformFileDialogHelper），
//   * 阻塞在嵌套事件循环里等用户选完（和 QFileDialog::getOpenFileName 一个语义），
//   * 拿到路径后立刻 deleteLater() 掉那个对象 —— 原生句柄当场释放，
//     和 QML 引擎没有任何长生命周期的绑定。
//
// 为什么不用 QFileDialog（Widgets）：本程序是 QGuiApplication，没有 QApplication，
// QFileDialog 是 QWidget/QDialog，跑不起来。这里用的是 QtGui 里那层
// **平台对话框抽象**（QPlatformTheme::createPlatformDialogHelper(FileDialog)），
// 它正是 QML 的 FileDialog 内部用的同一个东西，所以在 Windows 上就是系统原生的
// "打开/选择文件夹"对话框（IFileDialog），不是 Qt 自己画的。
//
// 拿不到平台对话框时（没装 QtGui 私有头、或平台插件不支持）：
// available() 返回 false，QML 会退回原来的 QML FileDialog 那条路 —— 功能不受影响，
// 只是又回到了"需要手动销毁"的老办法（代码里那份保留着，见 Main.qml/HomeWindow.qml）。
//
#ifndef CICADA_QT_APPFILEDIALOGS_H
#define CICADA_QT_APPFILEDIALOGS_H

#include <QObject>
#include <QString>

namespace cicadaqt {

    /*
     * 暴露给 QML 的单例（在 main.cpp 里用 qmlRegisterSingletonInstance 注册成
     * `AppDialogs`，QML 里 `import CicadaPlayer` 之后直接写 AppDialogs.openJsonFile(...)）。
     *
     * 四个方法都是**阻塞**的：内部跑一个嵌套事件循环等用户选完再返回。
     * 返回空串 = 用户取消 / 平台不支持；失败原因在 lastError() 里。
     */
    class AppFileDialogs : public QObject {
    Q_OBJECT
        /*
         * 这两个都是**属性**（不是 Q_INVOKABLE 方法）：
         * QML 里写 `AppDialogs.available` / `AppDialogs.lastError` 才拿到值。
         * 如果只声明成 Q_INVOKABLE，`AppDialogs.lastError` 在 QML 里得到的是
         * **函数对象**（日志里那句"原生文件对话框不可用：function() { [native code] }"
         * 就是这么来的）。
         */
        Q_PROPERTY(bool available READ available CONSTANT)
        Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    public:
        explicit AppFileDialogs(QObject *parent = nullptr);

        /* 平台原生文件对话框是否可用（QML 用它决定要不要退回 QML 的 FileDialog）。 */
        bool available() const;

        /* 最近一次失败的原因（空串 = 没有失败；用户取消不算失败）。 */
        QString lastError() const
        {
            return m_lastError;
        }

        /*
         * 选一个要播放的视频文件（支持的全部封装都列在过滤器里，最后留"所有文件"）。
         * startPath 为空时用上一次用过的目录（没有就用系统默认）。
         */
        Q_INVOKABLE QString openVideoFile(const QString &startPath = QString());

        /* 选一个清单 JSON 文件（清单对象对话框里那颗"从 .json 文件载入"）。 */
        Q_INVOKABLE QString openJsonFile(const QString &startPath = QString());

        /* 选一个目录（首页"添加文件夹"扫描视频库）。 */
        Q_INVOKABLE QString openDirectory(const QString &startPath = QString());

        /*
         * 当前是否有原生对话框正在显示。
         *
         * 给 main.cpp 的**卡死看门狗**用：走 "GUI 线程 exec()" 那条路时，原生模态
         * 对话框会阻塞在 Win32 的消息循环里（Qt 的定时器不派发），GUI 心跳自然停 ——
         * 看门狗会把"用户在文件选择框里翻了 10 秒"误判成"界面卡死"并打一堆线程栈。
         * 见 runDialog 里两种策略的说明。
         */
        static bool isDialogOpen();

    signals:
        /* lastError 属性变了（QML 绑定用；本轮界面是主动读，不依赖它）。 */
        void lastErrorChanged();

    private:
        /*
         * 真正干活的那个。fileMode / filters 直接对应 QFileDialogOptions 里的同名概念：
         *   existingFile  —— 选一个已存在的文件（Json/视频都用它）
         *   directoryOnly —— 选目录
         */
        QString runDialog(bool selectDirectory, const QString &title, const QString &startPath,
                          const QStringList &nameFilters);

        /* 写 lastError 并补发信号（值没变就不发）。 */
        void setLastError(const QString &error);

        /* 最近一次用户选过的目录：下次打开从那儿开始（和系统对话框的习惯一致）。 */
        QString m_lastDir;
        QString m_lastError;

        /*
         * 防重入：对话框是"阻塞在嵌套事件循环里"的，如果 QML 里连着调两次
         * （双击按钮、或者快捷键 + 点击同时到），第二个嵌套事件循环会让两个原生
         * 对话框同时挂着 —— 句柄和输入焦点都会乱。这里直接拒绝第二次调用。
         */
        bool m_busy = false;
    };

}// namespace cicadaqt

#endif// CICADA_QT_APPFILEDIALOGS_H
