//
// PreviewFrameSource 的实现，设计说明与三条硬约束见 PreviewFrameSource.h。
//

#define LOG_TAG "PreviewFrameSource"

#include "PreviewFrameSource.h"

/* 抽帧本体复用 VideoLibrary 里那段已经在跑的代码（见 VideoLibrary.cpp 的 grabFrameAt）：
   同一份实现同时给"首页封面"和"悬停预览"用，策略（抽哪一帧、失败怎么办）留在各自的调用方。 */
#include "VideoLibrary.h"

#include <utils/frame_work_log.h>

#include <QtCore/QMetaObject>
#include <QtCore/QSize>

namespace cicadaqt {

    /* 气泡只有 160x90；给 swscale 的格子尺寸和首页封面一致，一次缩到位。 */
    static const QSize kPreviewGrabSize(320, 180);

    PreviewFrameSource::PreviewFrameSource(QObject *context, ResultCallback callback)
        : m_context(context)
        , m_callback(std::move(callback))
    {
        /*
         * 一个专属工作线程：抽帧是阻塞的（打开文件 + seek + 解码），绝不能放在 GUI 线程上，
         * 也不能和播放共用线程。它只认"最新一个请求"，见 run()。
         */
        m_worker = std::thread([this]() {
            run();
        });
    }

    PreviewFrameSource::~PreviewFrameSource()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
            m_hasPending = false;
        }

        m_condition.notify_one();

        if (m_worker.joinable()) {
            /* 等这一轮抽帧退出：可能在解码一帧（几十毫秒级），不会无限等 —— run() 里
               唯一的阻塞源就是"有没有新请求"，收尾时它一定会看到 m_stopping。 */
            m_worker.join();
        }
    }

    QImage PreviewFrameSource::request(const QString &path, qint64 positionMs, int bucket)
    {
        const QString key = path + QLatin1Char('#') + QString::number(bucket);

        std::lock_guard<std::mutex> lock(m_mutex);

        const auto hit = m_cache.find(key);

        if (hit != m_cache.end()) {
            /* 命中：提到 LRU 最前，直接返回（同一档反复悬停不再抽帧）。 */
            m_cacheOrder.remove(key);
            m_cacheOrder.push_front(key);
            return hit->second;
        }

        /*
         * 未命中：把请求放进**唯一那个槽**（新请求覆盖旧的 —— 用户快速扫进度条时，
         * 中间那些档位本来就不需要出图，只有这样队列才是有界的），并把令牌 +1：
         * 更早那次在途的结果回来时令牌已经不是最新，会被丢掉（取消靠这个，不靠计时器）。
         */
        ++m_token;
        m_pending.path = path;
        m_pending.positionMs = positionMs;
        m_pending.bucket = bucket;
        m_pending.token = m_token;
        m_hasPending = true;
        m_condition.notify_one();

        return QImage();
    }

    void PreviewFrameSource::reset()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        ++m_token;              /* 在途结果一律作废 */
        m_hasPending = false;
        m_cache.clear();
        m_cacheOrder.clear();
    }

    void PreviewFrameSource::run()
    {
        for (;;) {
            Job job;

            {
                std::unique_lock<std::mutex> lock(m_mutex);

                m_condition.wait(lock, [this]() {
                    return m_stopping || m_hasPending;
                });

                if (m_stopping) {
                    return;
                }

                job = m_pending;
                m_hasPending = false;
            }

            /*
             * 解码（不持锁）。这是**独立会话**：自己开文件、自己 seek、自己解一帧，
             * 和播放那块解码器没有任何关系，也不从渲染队列取帧。
             */
            const QImage image = VideoLibrary::grabLocalFrame(job.path, job.positionMs, kPreviewGrabSize);

            if (image.isNull()) {
                /* 具体原因（打不开 / 没有视频流 / 没有解码器 / seek 不了 / 解不出帧）
                   已经在 grabLocalFrameAt 里逐条 AF_LOGW 过了，这里只补"所以这一档没有图"。 */
                AF_LOGW("hover preview: no frame at %lld ms (bucket %d) in \"%s\"; the bubble stays empty\n",
                        static_cast<long long>(job.positionMs), job.bucket, job.path.toUtf8().constData());
            }

            bool latest = false;

            {
                std::lock_guard<std::mutex> lock(m_mutex);

                if (!image.isNull()) {
                    /*
                     * 结果一律入缓存（按"文件#档位"），即使它已经过期 —— 内容对那个档位仍然有效，
                     * 用户再悬停回去时就能立刻出图。缓存有界：满了从 LRU 尾巴上丢。
                     */
                    const QString key = job.path + QLatin1Char('#') + QString::number(job.bucket);

                    m_cache[key] = image;
                    m_cacheOrder.remove(key);
                    m_cacheOrder.push_front(key);

                    while (static_cast<int>(m_cache.size()) > kCacheLimit && !m_cacheOrder.empty()) {
                        const QString victim = m_cacheOrder.back();
                        m_cacheOrder.pop_back();
                        m_cache.erase(victim);
                    }
                }

                /* 只有"最新那一次请求"的结果才投递出去。 */
                latest = (job.token == m_token) && !m_stopping;
            }

            if (!latest || m_context == nullptr) {
                continue;
            }

            if (!image.isNull()) {
                AF_LOGD("hover preview: frame at %lld ms (bucket %d) is ready (independent session, "
                        "software decode)\n", static_cast<long long>(job.positionMs), job.bucket);
            }

            /*
             * 投递回 GUI 线程。回调按**值**拷进 lambda（不引用本对象），并且以 m_context 作为
             * 接收者：那个对象析构之后，Qt 会把还没执行的投递丢掉 —— 所以回调里碰它的成员是安全的。
             */
            ResultCallback callback = m_callback;

            QMetaObject::invokeMethod(m_context, [callback, image, job]() {
                if (callback) {
                    callback(image, job.bucket, job.positionMs, job.path);
                }
            }, Qt::QueuedConnection);
        }
    }

}// namespace cicadaqt
