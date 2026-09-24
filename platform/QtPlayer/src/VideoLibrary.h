// ===========================================================================
// VideoLibrary —— 本地视频库（首页列表 + 播放器的播放列表共用这一份数据）
//
// 职责：
//   1. 递归扫描用户拖进来 / 选中的文件夹，收集视频文件（带深度上限，防止误选 C:\ 卡死）
//   2. 给每个视频抽**首帧**当缩略图，缓存到本地（抽一次就够，二次启动直接读缓存）
//   3. 作为播放列表：上一集 / 下一集 / 选集 都按这个顺序走
//
// 【为什么放在 Qt 层】扫描、缩略图缓存、拖拽都依赖 Qt（QDir/QImage/QStandardPaths）；
// 弹幕引擎那边保持纯 C++ 不变。
// ===========================================================================
#ifndef CICADA_QT_VIDEOLIBRARY_H
#define CICADA_QT_VIDEOLIBRARY_H

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
/* QML_ELEMENT：让 QML 里可以直接写 VideoLibrary { }（和 DanmakuController 同一种注册方式）。
   少了这个头会报 "语法错误: 缺少";"(在"public"的前面)" —— 那个坑踩过。 */
#include <QtQml/qqmlregistration.h>

namespace cicadaqt {

    class VideoLibrary : public QObject {
        Q_OBJECT
        QML_ELEMENT

        /* 列表：每项 { path, name, dir, sizeBytes, thumb, durationMs } */
        Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
        Q_PROPERTY(int count READ count NOTIFY itemsChanged)
        /* 当前播放的是第几项（-1 = 没有） */
        Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
        /* 是否还有上一条 / 下一条（控制栏那两颗按钮的显示判据） */
        Q_PROPERTY(bool hasPrevious READ hasPrevious NOTIFY currentIndexChanged)
        Q_PROPERTY(bool hasNext READ hasNext NOTIFY currentIndexChanged)
        /* 扫描中（首页可以显示进度/禁用交互） */
        Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)

    public:
        explicit VideoLibrary(QObject *parent = nullptr);

        QVariantList items() const { return m_items; }
        int count() const { return m_items.size(); }
        int currentIndex() const { return m_currentIndex; }
        void setCurrentIndex(int index);
        bool hasPrevious() const { return m_currentIndex > 0 && m_currentIndex < m_items.size(); }
        bool hasNext() const { return m_currentIndex >= 0 && m_currentIndex + 1 < m_items.size(); }
        bool scanning() const { return m_scanning; }

        /* 扫一个文件夹（递归，深度上限 maxDepth）；重复添加同一文件不会重复入列 */
        Q_INVOKABLE int addFolder(const QString &path, int maxDepth = 3);
        /* 拖进来的可能是文件也可能是文件夹：文件直接加，文件夹按 addFolder 处理 */
        Q_INVOKABLE int addPaths(const QStringList &paths, int maxDepth = 3);
        Q_INVOKABLE void removeAt(int index);
        Q_INVOKABLE void clear();

        /* 播放列表导航（播放器窗口用；返回是否真的动了） */
        Q_INVOKABLE bool goPrevious();
        Q_INVOKABLE bool goNext();
        /* 第 index 项的文件路径（越界返回空串） */
        Q_INVOKABLE QString pathAt(int index) const;
        Q_INVOKABLE QString nameAt(int index) const;

    signals:
        void itemsChanged();
        void currentIndexChanged();
        void scanningChanged();

    private:
        /* 抽首帧写进缓存，返回缓存文件路径（失败返回空） */
        QString thumbnailFor(const QString &videoPath);
        void appendFile(const QString &absolutePath, const QString &rootDir);
        void scanDir(const QString &dir, const QString &rootDir, int depth, int maxDepth);

        QVariantList m_items;
        QList<QString> m_paths;          /* 已经入列的文件（绝对路径，去重用） */
        QList<QString> m_roots;          /* 用户加过的根目录（界面用；也用于去重提示） */
        int m_currentIndex = -1;
        bool m_scanning = false;
    };

}// namespace cicadaqt

#endif// CICADA_QT_VIDEOLIBRARY_H
