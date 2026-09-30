//
// Created by lifujun on 2019/7/23.
//

#ifndef FRAMEWORK_IVideoRender_H
#define FRAMEWORK_IVideoRender_H


#include <base/media/IAFPacket.h>
#include <functional>
#include <utils/CicadaJSON.h>

typedef bool (*videoRenderingFrameCB)(void *userData, IAFFrame *frame, const CicadaJSONItem &params);


class IVideoRender {

public:
    static const uint64_t FLAG_HDR = (1 << 0);
    static const uint64_t FLAG_DUMMY = (1 << 1);

    enum Rotate {
        Rotate_None = 0,
        Rotate_90 = 90,
        Rotate_180 = 180,
        Rotate_270 = 270
    };

    enum Flip {
        Flip_None,
        Flip_Horizontal,
        Flip_Vertical,
        Flip_Both,
    };

    enum Scale {
        Scale_AspectFit,
        Scale_AspectFill,
        Scale_Fill
    };

    static Rotate getRotate(int value)
    {
        switch (value) {
            case 90:
                return Rotate_90;
            case 180:
                return Rotate_180;
            case 270:
                return Rotate_270;
            default:
                return Rotate_None;
        }
    };

    class ScreenShotInfo {
    public:
        enum Format {
            UNKNOWN, RGB888,
        };
    public:
        Format format = UNKNOWN;

        int width = 0;
        int height = 0;

        int64_t bufLen = 0;
        char *buf = nullptr;

        ~ScreenShotInfo()
        {
            if (buf != nullptr) {
                free(buf);
                buf = nullptr;
            }
        }
    };


    class IVideoRenderFilter {
    public:
        virtual ~IVideoRenderFilter() = default;

        // a fbo or a frame
        virtual int push(void *) = 0;

        virtual int pull(void *) = 0;

    };

    class IVideoRenderListener {
    public:
        virtual void onFrameInfoUpdate(IAFFrame::AFFrameInfo &info, bool rendered) = 0;
        virtual ~IVideoRenderListener() = default;
    };

public:
    virtual ~IVideoRender() = default;

    /**
     * init render
     * @return
     */
    virtual int init() = 0;

    /**
     * clear screen to black.
     */
    virtual int clearScreen() = 0;

    /*
     * set background color
     */
    virtual void setBackgroundColor(uint32_t color) = 0;

    /**
     * set want draw frame.
     * @param frame
     */
    virtual int renderFrame(std::unique_ptr<IAFFrame> &frame) = 0;

    virtual void setListener(IVideoRenderListener *listener)
    {
        mListener = listener;
    }

    /**
     * set render rotate.
     * @param rotate
     */
    virtual int setRotate(Rotate rotate) = 0;

    /**
     * set render flip.
     * @param flip
     */
    virtual int setFlip(Flip flip) = 0;

    /**
     * 【色觉辅助滤镜 / 回退点 R1】设置 3x3 颜色矩阵（行主序，单位矩阵 = 关闭）。
     *
     * **非纯虚 + 空默认实现**：这是刻意的 —— Qt / Apple / Dummy 等渲染器一行都不用改，
     * 也不需要任何平台 #ifdef。只有 GLRender 覆写它。
     *
     * 已知限制（不解决，写明即可）：隧道/direct 渲染（getFlags() & FLAG_DUMMY）没有着色器，
     * 滤镜无效；GLRender::captureScreen 抓的是加滤镜之前的画面。
     *
     * @param matrix 9 个 float，行主序
     */
    virtual void setColorMatrix(const float matrix[9])
    {
        (void) matrix;
    }

    /**
     * set render scale.
     * @param scale
     */
    virtual int setScale(Scale scale) = 0;

    /**
     * set the playback speed, rend use it to improve render smooth
     * @param speed
     */
    virtual void setSpeed(float speed) = 0;

    /**
     * set window size when window size changed.
     * @param windWith
     * @param windHeight
     */
    virtual void setWindowSize(int windWith, int windHeight) {

    }

    virtual void surfaceChanged() {

    }

    /**
     * set display view
     * @param view
     */
    virtual int setDisPlay(void *view)
    {
        return 0;
    }


    virtual void captureScreen(std::function<void(uint8_t *, int, int)> func)
    {
        func(nullptr,0,0);
    }


    virtual void *getSurface(bool cached)
    {
        return nullptr;
    }

    virtual float getRenderFPS() = 0;

    virtual void invalid(bool invalid)
    {
        mInvalid = invalid;
    }
    virtual uint64_t getFlags() = 0;

    virtual void setVideoRenderingCb(videoRenderingFrameCB cb, void *userData)
    {
        mRenderingCb = cb;
        mRenderingCbUserData = userData;
    }

    class videoProcessTextureCb {
    public:
        videoProcessTextureCb() = default;

        virtual ~videoProcessTextureCb() = default;

        /**
         * @param type      TEXTURE_YUV 0, TEXTURE_RGBA 1
         * @return
         */
        virtual bool init(int type) = 0;

        virtual bool needProcess() = 0;

        virtual bool push(std::unique_ptr<IAFFrame> &textureFrame) = 0;

        virtual bool pull(std::unique_ptr<IAFFrame> &textureFrame) = 0;
    };

    virtual void setVideoProcessTextureCb(videoProcessTextureCb *cb)
    {
        mProcessTextureCb = cb;
    }

    /*
     * 【新增·vtable 末尾】放掉本渲染器手上**所有的解码帧**，同步完成（返回即已放掉）。
     *
     * 为什么必须单独有这么一个动作，现有两条路都不够：
     *   · renderFrame(nullptr) 只是**登记**一次 flush —— AFActiveVideoRender 把它记成
     *     mNeedFlushSize，真正的丢帧发生在**下一次 VSync 回调**里（onVSync 开头那段
     *     while）。于是"解码器已经关了、渲染器手里那一帧还钉着解码器的输出缓冲"
     *     这个顺序一直存在；
     *   · clearScreen() 会把画布清黑（SDL 路径真的清屏），切档/seek 上用不了。
     *
     * 为什么这一帧如此要紧：硬解帧的 buf[] 里握着解码器的输出缓冲，D3D11VA 更极端 ——
     * 整池 surface 装在**同一张** ID3D11Texture2D 里（1080p 20 片 NV12 约 62MB、
     * 4K 约 249MB），只要还有**一帧**活着，整张纹理数组就释放不掉。所以
     * "关解码器之前先把手上的帧全放掉"是表面池能否随解码器一起回收的前提。
     *
     * 调用契约：播放器在**关闭或重建视频解码器之前**调用它
     * （SuperMediaPlayer::FlushVideoPath() / rebuildVideoDecoder()），实现里因此
     * **不得**把放帧推迟到别的线程、别的时机；返回时本渲染器不得再持有任何解码帧。
     *
     * 默认空实现：压根不用帧的渲染器（dummy / tunnel / 只拿 surface 的那些）
     * 一行都不用改，也不需要任何平台分支。
     */
    virtual void releaseFrames()
    {}

protected:
    bool mInvalid{false};
    IVideoRenderListener *mListener{nullptr};

    videoRenderingFrameCB mRenderingCb{nullptr};
    void *mRenderingCbUserData{nullptr};

    videoProcessTextureCb *mProcessTextureCb{nullptr};
};


#endif //FRAMEWORK_IVideoRender_H
