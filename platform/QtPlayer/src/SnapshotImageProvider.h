//
// SnapshotImageProvider —— 把 CicadaPlayerItem 截到的快照暴露给 QML 的 Image。
//
// 【它解决什么问题】
// QML 的 Image 只认 URL，而快照是 C++ 侧一帧内存里的像素（框架 CaptureScreen 回调
// 给出来的 width*height*4 的 RGBA）。QQuickImageProvider 就是 Qt 为"像素由 C++ 产生、
// 由 QML 显示"准备的桥：注册成 image://snapshot/<rev> 之后，
//     Image { source: "image://snapshot/" + player.snapshotRevision }
// 只要 revision 变了，Qt 就会来 requestImage() 取一次新图。
//
// 【为什么是"进程内一份"而不是每个 item 一份】
//   * 快照只有进度条悬停气泡一个消费者，而同一时刻只可能有一个窗口在被悬停；
//     要为它建一套"按 item id 路由"的注册表（provider 得拿到所有 item 的指针、
//     还得处理 item 析构）纯属多余的复杂度。
//   * 于是取最省事的做法：**函数内静态量**（inline 函数里的 static 在 C++ 里是
//     进程内唯一的一份）。这样本文件可以做成**纯头文件** —— 不用新增 .cpp、
//     也就**不用改 CMakeLists**（provider 要同时被 main.cpp 和 CicadaPlayerItem.cpp
//     看到：前者注册它，后者往里发布）。
//   * 多窗口语义：**后发布的赢**（last writer wins）。A、B 两个播放器窗口各自悬停时，
//     谁最后请求并收到快照，两个窗口的气泡都会显示那一张。这是有意的取舍：进度条悬停
//     本来就是"当前正在操作的那个窗口"的行为，同屏同时悬停两个窗口在物理上做不到。
//
// 【线程 —— 这一点必须记住】
// requestImage() **不保证在 GUI 线程**：
//   * QML 的 Image 开了 asynchronous: true（进度条气泡就是），Qt 会在**工作线程**里调它；
//   * 场景图的纹理更新也可能发生在**渲染线程**上。
// 而发布（publish）在 GUI 线程。所以两边共用一把锁：QImage 的隐式共享只保证引用计数
// 本身安全，**不保证**"一边赋新图、一边读旧图"这种竞争。
//
// 【取不到图】
// 返回**空 QImage**（不抛异常、不造占位图）：QML 侧表现为"这个 URL 还没有内容"，
// 气泡里就是一个空框 —— 绝不出现破图。id 里的数字只是 revision（见下）。
//
#ifndef CICADA_QT_SNAPSHOTIMAGEPROVIDER_H
#define CICADA_QT_SNAPSHOTIMAGEPROVIDER_H

#include <QtGui/QImage>
#include <QtCore/QMutex>
#include <QtCore/QMutexLocker>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtQuick/QQuickImageProvider>

namespace cicadaqt {

    /*
     * Image 类型的 provider（不是 Pixmap/Texture）：直接把 QImage 交给 Qt，
     * 由 Qt 自己决定怎么上传成纹理、什么时候缓存。
     */
    class SnapshotImageProvider : public QQuickImageProvider {
    public:
        SnapshotImageProvider()
            : QQuickImageProvider(QQuickImageProvider::Image)
        {
        }

        /*
         * QML 侧 `image://snapshot/<rev>` 的每一次请求都到这里（id 就是 "<rev>" 那段）。
         *
         * id 里的数字**只是 revision**：它的唯一作用是让 URL 每次都不同，从而让 Qt
         * 知道"这是新图、别拿缓存糊弄我"。provider 不解析它、也不需要它 —— 手里永远
         * 只有"最近一张"。
         *
         * requestedSize 是 QML 侧 Image 声明的 sourceSize（本工程的进度条气泡没设，
         * 通常是个空/非法尺寸）。只有它确实有效时才缩放；否则原样返回 —— 气泡的
         * 160x90 由 Image 的 fillMode: Stretch 负责拉伸，比在这里缩放更省事，
         * 也不会因为 provider 里的缩放把图裁掉。
         *
         * 返回空 QImage 表示"暂时没有画面"（界面保持空框）。
         */
        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
        {
            /* id 不参与取值，只用于让 URL 变化；留着参数名是为了和 Qt 的签名一致。 */
            Q_UNUSED(id);

            QImage image;

            {
                QMutexLocker locker(&store().mutex);
                /* 隐式共享：这里只是引用计数 +1，不拷贝像素。 */
                image = store().image;
            }

            if (size != nullptr) {
                *size = image.size();
            }

            if (!image.isNull() && requestedSize.isValid()
                    && image.size() != requestedSize) {
                image = image.scaled(requestedSize, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
            }

            return image;
        }

        /*
         * 发布"最近一张快照"。**只能在 GUI 线程调用**（CicadaPlayerItem::notifySnapshot
         * 是被 queued 投递到 GUI 线程之后才调它的），但读取方可能在渲染/工作线程，
         * 所以这里和 requestImage() 用同一把锁。
         *
         * 传进来的那一份**已经缩小过**（item 侧做的，见 CicadaPlayerItem::notifySnapshot）：
         * 气泡只有 160x90，没必要让场景图去上传一张 4K 纹理。
         */
        static void publish(const QImage &image)
        {
            QMutexLocker locker(&store().mutex);
            store().image = image;
        }

    private:
        /*
         * 进程内唯一的一份"最近快照"。
         *
         * 用**函数内静态量**而不是类的静态成员：类的静态成员在头文件里定义会变成
         * 每个编译单元一份（重复符号），要么加 inline（C++17）要么另开 .cpp ——
         * 而 inline 函数里的 static 天生就是全进程一份，最干净。
         */
        struct Store {
            QMutex mutex;
            QImage image;
        };

        static Store &store()
        {
            static Store instance;
            return instance;
        }
    };

}// namespace cicadaqt

#endif// CICADA_QT_SNAPSHOTIMAGEPROVIDER_H
