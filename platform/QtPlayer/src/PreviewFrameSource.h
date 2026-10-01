// ===========================================================================
// PreviewFrameSource —— 进度条悬停预览的"按位置独立抽帧"。
//
// 【它解决什么问题】
// 原来悬停预览走的是框架的 CaptureScreen，而那条路抓的是**当前正在渲染的那一帧**：
//     ProgressRow.qml:357-388  requestSnapshotForFrame()/requestSnapshotThrottled()
//         → bar.player.requestSnapshot()（**不带位置**，5 秒一档只决定"什么时候再请求一次"）
//     CicadaPlayerItem.cpp:1678-1698  requestSnapshot() → m_player->CaptureScreen()
//     CicadaVideoRender.cpp:367-379  device_captureScreen() 读 m_lastFrame
//         （那是 deviceRenderFrame() 在 CicadaVideoRender.cpp:201-204 留下的"正在显示的那一帧"）
// 所以鼠标悬停在任何位置，拿到的都是"此刻播放位置"的画面 —— 用户看到的就是
// "预览图始终是一段时间内的，不是整个视频的"；暂停时更是完全不动。
//
// 这个类把预览与播放**彻底解耦**：按悬停位置（毫秒）自己去本地文件 seek + 解一帧。
//
// 【三条硬约束怎么满足】
//   * 不抢播放资源：抽帧是**独立会话**（VideoLibrary::grabLocalFrame → 自己的
//     avformat_open_input + AVCodecContext），既不碰播放的解码器，也不从渲染队列偷帧。
//     解码方式是**软解**（libavformat/libavcodec/libswscale）—— 如实说明：它不是 VideoToolbox
//     硬解，但它在播放之外，完全不影响播放那条 VideoToolbox 零拷贝的路；
//   * 不阻塞界面与播放：抽帧只在**一个专属工作线程**上跑；结果经 Qt 队列投递回 GUI 线程；
//   * 有界：待处理请求只有**一个槽**（新请求覆盖旧的 = 自动合并，扫进度条时不会堆积），
//     缓存是固定条数的 LRU（见 kCacheLimit）；取消不用计时器/看门狗，只用**请求令牌**这种
//     纯状态：回来的结果令牌不是最新就丢掉。
//
// 【失败语义（最重要的一条）】
// 抽不到就**没有图**：调用方在请求时就把"当前显示的是不是这一档"置成否（界面显示空框占位），
// 抽帧失败不补任何东西 —— 绝不允许拿相邻位置的旧图冒充（那正是用户抱怨的现象）。
//
// 【线程约定】
//   request() / reset() / 构造 / 析构：**GUI 线程**（和调用方的 UI 状态一致）；
//   工作线程只碰自己的工作副本 + 加锁的缓存。
// ===========================================================================
#ifndef CICADA_QT_PREVIEWFRAMESOURCE_H
#define CICADA_QT_PREVIEWFRAMESOURCE_H

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtGui/QImage>

#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <thread>

namespace cicadaqt {

    class PreviewFrameSource {
    public:
        /*
         * 抽帧结果回调（**GUI 线程**上调用）。
         * image 为空 = 这一档抽不到（调用方必须按"没有图"处理，不许拿别的帧顶替）。
         */
        using ResultCallback = std::function<void(const QImage &image, int bucket,
                                     qint64 positionMs, const QString &path)>;

        /*
         * context：结果投递用的 QObject（必须是 GUI 线程上的对象，通常是拥有本对象的那个 item）。
         *          它析构之后，尚未投递的结果会被 Qt 丢掉 —— 所以回调里碰它的成员是安全的。
         */
        PreviewFrameSource(QObject *context, ResultCallback callback);
        ~PreviewFrameSource();

        PreviewFrameSource(const PreviewFrameSource &) = delete;
        PreviewFrameSource &operator=(const PreviewFrameSource &) = delete;

        /*
         * GUI 线程：请求"path 文件在 positionMs 处"的一帧，bucket 是界面的档位（同一档不重复抽）。
         *   命中缓存 → 直接返回那张图（同步，没有空窗）；
         *   没命中   → 交给工作线程并返回**空图**（调用方据此把界面置成占位）。
         */
        QImage request(const QString &path, qint64 positionMs, int bucket);

        /* GUI 线程：丢掉在途结果并清缓存（换片源 / 收尾时用）。 */
        void reset();

    private:
        void run();

        struct Job {
            QString path;
            qint64 positionMs = 0;
            int bucket = -1;
            quint64 token = 0;
        };

        /* 缓存条数上限：320x180 RGB888 一张约 173KB，32 张 ≈ 5.5MB，有界。 */
        static constexpr int kCacheLimit = 32;

        QObject *m_context = nullptr;
        ResultCallback m_callback;

        std::thread m_worker;
        std::mutex m_mutex;
        std::condition_variable m_condition;
        Job m_pending;
        bool m_hasPending = false;
        bool m_stopping = false;
        quint64 m_token = 0;

        std::map<QString, QImage> m_cache;
        /* 最近使用的键在最前面（LRU：满了从尾巴上丢） */
        std::list<QString> m_cacheOrder;
    };

}// namespace cicadaqt

#endif// CICADA_QT_PREVIEWFRAMESOURCE_H
