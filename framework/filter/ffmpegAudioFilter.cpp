//
// Created by moqi on 2018/10/30.
//
#define LOG_TAG "ffmpegAudioFilter"

#include <utils/timer.h>
#include <utils/frame_work_log.h>
#include <mutex>
#include <base/media/AVAFPacket.h>
#include "ffmpegAudioFilter.h"
#include "utils/AutoAVFrame.h"
#include <utils/ffmpeg_utils.h>
#include <cstdint>
#include <cinttypes>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
}

#define MAX_INPUT_BUFFER_COUNT 2
#define MAX_OUTPUT_BUFFER_COUNT 2
//#define DUMP_PCM
#ifdef DUMP_PCM
    static int fd;

    #include <fcntl.h>
    #include <unistd.h>

#endif

namespace Cicada {

    /*
     * 【音量均衡】三档预设 —— docs/audio-loudness-modes.md §5 换算表的直接落地：
     *   threshold / knee 由 Web 端（DynamicsCompressorNode）的 dB 值换算成线性幅度：
     *     标准：threshold=10^(-18/20)=0.125893，knee=10^(24/20)=15.85 → 夹上限 8；
     *     高动态：threshold=10^(-32/20)=0.025119，knee=10^(12/20)=3.981；
     *   attack / release 由秒 × 1000 成毫秒；ratio 同范围原样。
     * 关闭档 ratio=1 + knee=1：acompressor 的 output_gain 在 ratio=1 时数学上恒等直通
     * （含软拐点区间，文档 §4.3），所以"关闭"不需要拆图 —— 图常驻、档位切换零重建。
     * threshold/attack/release 在关闭档写的什么值都无所谓（ratio=1 时根本走不到），
     * 填标准档的值只为表整齐。
     */
    static const char *kLoudnessPresets[3][5] = {
        /* threshold  knee    ratio  attack  release */
        {"0.125893", "1",     "1",   "3",    "250"},   /* 0 关闭：ratio=1 直通 */
        {"0.125893", "8",     "12",  "3",    "250"},   /* 1 标准：提前触发 + 高比率，拉平响度 */
        {"0.025119", "3.981", "4",   "10",   "400"},   /* 2 高动态：深阈值 + 低比率，只压峰值 */
    };
    /* 与 kLoudnessPresets 逐列对应的 acompressor 参数名（send_command 用） */
    static const char *kLoudnessParamNames[5] = {"threshold", "knee", "ratio", "attack", "release"};

    ffmpegAudioFilter::ffmpegAudioFilter(const format &srcFormat, const format &dstFormat, bool active)
        : IAudioFilter(srcFormat, dstFormat, active)
    {
        // avfilter_register_all was removed in FFmpeg 5.0 (filters self-register).
#ifdef DUMP_PCM
        fd = open("out.pcm", O_CREAT | O_RDWR, 0666);
#endif
    }

    ffmpegAudioFilter::~ffmpegAudioFilter()
    {
        delete mPThread;
        mPThread = nullptr;
        avfilter_graph_free(&m_pFilterGraph);
        flush();
    }

    bool ffmpegAudioFilter::isSupported(string capacity)
    {
        if (capacity == "atempo") {
            return true;
        }

        return false;
    }

    bool ffmpegAudioFilter::setOption(const string &key, const string &value, const string &capacity)
    {
        if (capacity == "atempo") {
            if (key == "rate") {
                if (mRate != atof(value.c_str())) {
                    std::lock_guard<std::mutex> uMutex(mMutexRate);
                    mRate = atof(value.c_str());

                    if (m_pFilterGraph) {
                        avfilter_graph_send_command(m_pFilterGraph, "atempo", "tempo", value.c_str(), nullptr, 0, 0);
                    }
                }

                return true;
            }
        } else if (capacity == "volume") {
            if (mVolume != atof(value.c_str())) {
                mVolume = atof(value.c_str());

                if (m_pFilterGraph) {
                    avfilter_graph_send_command(m_pFilterGraph, "volume", "volume", value.c_str(), nullptr, 0, 0);
                }
            }

            return true;
        } else if (capacity == "loudness") {
            /*
             * 【音量均衡】档位切换（0 关闭 / 1 标准 / 2 高动态）。
             * 必须持 mMutexRate（与 atempo 分支同一把锁、同一先例）：send_command 和
             * FilterLoop 线程并发访问 graph 不是线程安全的（volume 分支没加锁属历史
             * 遗留，勿仿照）。图还没建时只记 mLoudnessMode —— init() 会按它构建。
             */
            if (key == "mode") {
                int mode = atoi(value.c_str());

                if (mode < 0 || mode > 2) {
                    return false;
                }

                if (mode != mLoudnessMode.load()) {
                    std::lock_guard<std::mutex> uMutex(mMutexRate);
                    mLoudnessMode = mode;

                    if (m_pFilterGraph) {
                        applyLoudnessToGraph();
                    }
                }

                return true;
            }
        }

        return false;
    }

    void ffmpegAudioFilter::applyLoudnessToGraph()
    {
        /*
         * 【音量均衡】档位热更新：acompressor 全参数带运行时 R 标志 + process_command
         * （FFmpeg 9.0 af_sidechaincompress.c:75-95/:435），逐参数 send_command 即可，
         * 与 atempo/volume 的热更新是同一机制。attack/release 自带包络平滑，参数阶跃
         * 通常无爆音（文档 §6 注意点 ③）。
         * 调用方（setOption）已持 mMutexRate；只应在本函数内被调用。
         */
        int mode = mLoudnessMode.load();

        if (mode < 0 || mode > 2) {
            mode = 0;
        }

        const char **preset = kLoudnessPresets[mode];

        for (int i = 0; i < 5; ++i) {
            avfilter_graph_send_command(m_pFilterGraph, "acompressor", kLoudnessParamNames[i], preset[i], nullptr, 0, 0);
        }
    }

    int ffmpegAudioFilter::createSrcBufferFilter()
    {
        char ch_layout[64] = {0};
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        mAbuffer_ctx = avfilter_graph_alloc_filter(m_pFilterGraph, abuffer, "src");

        if (mAbuffer_ctx == nullptr) {
            return -EINVAL;
        }

        AVRational r;
        r.den = 1000000;
        r.num = 1;

        /* AVChannelLayout replaced channel_layout/channels in FFmpeg 5.1. */
        AVChannelLayout srcLayout{};
        if (mSrcFormat.channel_layout != 0) {
            av_channel_layout_from_mask(&srcLayout, mSrcFormat.channel_layout);
        } else {
            av_channel_layout_default(&srcLayout, mSrcFormat.channels);
        }
        av_channel_layout_describe(&srcLayout, ch_layout, sizeof(ch_layout));
        av_channel_layout_uninit(&srcLayout);

        /* Set the filter options through the AVOptions API. */
        av_opt_set(mAbuffer_ctx, "channel_layout", ch_layout, AV_OPT_SEARCH_CHILDREN);
        av_opt_set(mAbuffer_ctx, "sample_fmt", av_get_sample_fmt_name((enum AVSampleFormat) mSrcFormat.format), AV_OPT_SEARCH_CHILDREN);
        av_opt_set_q(mAbuffer_ctx, "time_base", r, AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(mAbuffer_ctx, "sample_rate", mSrcFormat.sample_rate, AV_OPT_SEARCH_CHILDREN);
        /* Now initialize the filter; we pass NULL options, since we have already
        * set all the options above. */
        int err = avfilter_init_str(mAbuffer_ctx, nullptr);

        if (err < 0) {
            AF_LOGE("Could not initialize the abuffer filter.\n");
        }

        return err;
    }

    AVFilterContext *ffmpegAudioFilter::createFilter(const char *name, const char *opt)
    {
        const AVFilter *filter = avfilter_get_by_name(name);
        AVFilterContext *filterContext = avfilter_graph_alloc_filter(m_pFilterGraph, filter, name);

        if (filterContext == nullptr) {
            return nullptr;
        }

        int err = avfilter_init_str(filterContext, opt);

        if (err < 0) {
            AF_LOGE("Could not initialize the %s filter.\n", opt);
            avfilter_free(filterContext);
            return nullptr;
        }

        return filterContext;
    }

    int ffmpegAudioFilter::addFilter(AVFilterContext **current, const char *name, const char *options_str)
    {
        int err;
        AVFilterContext *filterContext = createFilter(name, options_str);

        if (filterContext == nullptr) {
            AF_LOGW("%s filter create fail\n", name);
            return -EINVAL;
        } else {
            err = avfilter_link(*current, 0, filterContext, 0);

            if (err < 0) {
                AF_LOGW("%s filter link fail\n", name);
                return -EINVAL;
            }

            *current = filterContext;
        }

        return err;
    }

    int ffmpegAudioFilter::init(uint64_t flags)
    {
        int err;
        mFlags = flags;
        m_pFilterGraph = avfilter_graph_alloc();

        if (m_pFilterGraph == nullptr) {
            return -ENOMEM;
        }

        AVFilterContext *current = nullptr;
        err = createSrcBufferFilter();

        if (err < 0) {
            return err;
        }

        mAbuffersink_ctx = createFilter("abuffersink", nullptr);

        if (mAbuffersink_ctx == nullptr) {
            return -EINVAL;
        }

        current = mAbuffer_ctx;
        char options_str[1024];
        bool needAFormat = false;
        // for now enlarge only
        if (flags & A_FILTER_FLAG_VOLUME) {
            snprintf(options_str, sizeof(options_str), "volume=%f", std::max(mVolume, 1.0));
            err = addFilter(&current, "volume", options_str);
        }

        if (err == 0 //volume would add a aformat filter too
                || mSrcFormat.sample_rate != mDstFormat.sample_rate
                || mSrcFormat.format != mDstFormat.format
                || mSrcFormat.channels != mDstFormat.channels) {
            needAFormat = true;
        }

        /*
         * 【音量均衡】acompressor 常驻（volume 段之后、atempo 段之前，文档 §6 ②）：
         *   * 图按**当前档位**构建（mLoudnessMode 由 setOption 提前记好），所以重建图
         *     （seek/flush、懒建图）后档位自动恢复，不需要重发命令；
         *   * 关闭档构建的也是 ratio=1 直通 —— 切换永远不重建图、不产生音频间隙；
         *   * 不改采样格式，故对上面的 needAFormat 判断无影响。
         * 【预留/降级】返回值故意不检查（不能学 atempo 段的 return）：addFilter 失败时
         * *current 保持原值，后续滤镜直接接在前一个上 —— 当前 Windows FFmpeg 构建里
         * acompressor 未编入（CONFIG_ACOMPRESSOR_FILTER=0），这条路径实测走通：
         * 仅一条 AF_LOGW、图照常建成、主音频路径零影响（音量均衡暂时无效果，属预期）。
         * 白名单已加 acompressor（player_ffmpeg_config.sh），交叉编译 FFmpeg 后
         * 这里不需要任何改动即自动生效。
         */
        if (flags & A_FILTER_FLAG_LOUDNESS) {
            int mode = mLoudnessMode.load();

            if (mode < 0 || mode > 2) {
                mode = 0;
            }

            const char **preset = kLoudnessPresets[mode];
            snprintf(options_str, sizeof(options_str),
                     "threshold=%s:knee=%s:ratio=%s:attack=%s:release=%s:makeup=1:detection=rms:link=average:mix=1",
                     preset[0], preset[1], preset[2], preset[3], preset[4]);
            addFilter(&current, "acompressor", options_str);
        }

        if (flags & A_FILTER_FLAG_TEMPO) {
            snprintf(options_str, sizeof(options_str), "tempo=%f", mRate.load());
            addFilter(&current, "atempo", options_str);
        }

        if (needAFormat) {
            char dstLayoutStr[64] = {0};
            AVChannelLayout dstLayout{};
            if (mDstFormat.channel_layout != 0) {
                av_channel_layout_from_mask(&dstLayout, mDstFormat.channel_layout);
            } else {
                av_channel_layout_default(&dstLayout, mDstFormat.channels);
            }
            av_channel_layout_describe(&dstLayout, dstLayoutStr, sizeof(dstLayoutStr));
            av_channel_layout_uninit(&dstLayout);
            snprintf(options_str, sizeof(options_str),
                     "sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
                     av_get_sample_fmt_name((enum AVSampleFormat) mDstFormat.format),
                     mDstFormat.sample_rate,
                     dstLayoutStr);
            addFilter(&current, "aformat", options_str);
        }

        err = avfilter_link(current, 0, mAbuffersink_ctx, 0);
        err = avfilter_graph_config(m_pFilterGraph, nullptr);

        std::string value = std::to_string(mRate);
        avfilter_graph_send_command(m_pFilterGraph, "atempo", "tempo", value.c_str(), nullptr, 0, 0);
        value = std::to_string(mVolume);
        avfilter_graph_send_command(m_pFilterGraph, "volume", "volume", value.c_str(), nullptr, 0, 0);

        mFirstPts = INT64_MIN;
        mDeltaPts = 0;
        mLastInputPts = INT64_MIN;
        mLastInPutDuration = 0;

        if (mActive && mPThread == nullptr) {
            mPThread = NEW_AF_THREAD(FilterLoop);
            mPThread->start();
        }

        return 0;
    }

    int ffmpegAudioFilter::push(std::unique_ptr<IAFFrame> &frame, uint64_t timeOut)
    {
        if (mInPut.read_available() >= MAX_INPUT_BUFFER_COUNT || mOutPut.read_available() > MAX_OUTPUT_BUFFER_COUNT) {
            if (!mActive) {
                FilterLoop();
            }
            return -EAGAIN;
        }

        int64_t pts = frame->getInfo().pts;

        if (pts != INT64_MIN) {
            if (mLastInputPts != INT64_MIN) {
                int64_t deltaPts = pts - (mLastInputPts + mLastInPutDuration);

                if (llabs(deltaPts) > mLastInPutDuration / 2) {
                    /*
                     * 普通编码抖动（几十毫秒）可以通过 mDeltaPts 平滑掉；但
                     * DASH/HLS seek 后有些 segment 会携带完全不同的时间轴，
                     * deltaPts 可能达到几十秒。这里必须把“时间轴跳变”反向抵消：
                     * 输出时间 = 滤波器时间 + mFirstPts + mDeltaPts，因此输入
                     * 向前跳 deltaPts 时应减去该值（而不是继续累加正值）。日志中
                     * mDeltaPts=50154667/87168001 正是原实现把正向跳变越加越大的
                     * 证据，最终造成音频整体领先视频几十秒。
                     *
                     * 大跳变只修正一次，后续帧继续沿用同一 offset；本地文件和
                     * 其它平台的小幅连续 PTS 修正逻辑保持不变。
                     */
                    constexpr int64_t kLargeTimelineDiscontinuityUs = 5 * 1000 * 1000;
                    if (llabs(deltaPts) >= kLargeTimelineDiscontinuityUs) {
                        AF_LOGW("audio timeline discontinuity %lld us, re-anchor filter at pts=%lld\n",
                                (long long) deltaPts, (long long) pts);
                        mDeltaPts -= deltaPts;
                    } else {
                        mDeltaPts += deltaPts;
                        AF_LOGD("mDeltaPts is %lld\n", mDeltaPts);
                    }
                }
            }

            mLastInputPts = pts;
        }

        mLastInPutDuration = static_cast<int64_t>(frame->getInfo().audio.nb_samples * 1000 /
                             (float) (frame->getInfo().audio.sample_rate / 1000));
        mPts.push(pts);
        mInPut.push(frame.release());
        if (!mActive) {
            FilterLoop();
        }
        return 0;
    }

    int ffmpegAudioFilter::FilterLoop()
    {
        int ret;
        IAFFrame *frame;

        while (!mInPut.empty() && mOutPut.read_available() < MAX_OUTPUT_BUFFER_COUNT) {
            frame = mInPut.front();
            mInPut.pop();
            IAFFrame::audioInfo info = frame->getInfo().audio;

            if (info.sample_rate != mSrcFormat.sample_rate || info.format != mSrcFormat.format || info.channels != mSrcFormat.channels) {
                /* reset the filer will drop the data in filter cache, so it is not expected
             */
                assert(0);
                mSrcFormat = info;

                if (m_pFilterGraph) {
                    avfilter_graph_free(&m_pFilterGraph);
                }
            }

            {
                std::lock_guard<std::mutex> uMutex(mMutexRate);

                if (m_pFilterGraph == nullptr) {
                    ret = init(mFlags);

                    if (ret < 0) {
                        AF_LOGE("init error\n");
                        return ret;
                    }
                }
            }

            int64_t pts = dynamic_cast<AVAFFrame *>(frame)->getInfo().pts;

            int64_t timePosition = frame->getInfo().timePosition;
            int64_t utcTime = frame->getInfo().utcTime;

            if (mFirstPts == INT64_MIN) {
                mFirstPts = pts;
            }

            ret = av_buffersrc_add_frame(mAbuffer_ctx, getAVFrame(frame));

            if (ret < 0) {
                AF_LOGE("Error submitting the frame to the filtergraph: %s", getErrorString(ret));
                delete frame;
                return ret;
            }

            /* Get all the filtered output that is available. */
            while (true) {
                AVFrame *avFrame = nullptr;

                if (frame == nullptr) {
                    avFrame = av_frame_alloc();
                    frame = new AVAFFrame(&avFrame);
                }

                avFrame = getAVFrame(frame);
                avFrame->pts = pts;
                ret = av_buffersink_get_frame(mAbuffersink_ctx, avFrame);

                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    delete frame;
                    break;
                }

                if (ret < 0) {
                    delete frame;
                    return ret;
                }

                AVRational tb = av_buffersink_get_time_base(mAbuffersink_ctx);

                if (avFrame->pts != AV_NOPTS_VALUE) {
                    avFrame->pts = av_rescale_q(avFrame->pts, tb, av_get_time_base_q());
                }

                if (mReferInputPTS) {
                    if (!mPts.empty()) {
                        pts = mPts.front();
                        mPts.pop();
                    }

                    avFrame->pts = pts;
                } else if (avFrame->pts != AV_NOPTS_VALUE) {
                    avFrame->pts *= mRate;

                    if (mFirstPts != INT64_MIN) {
                        avFrame->pts += mFirstPts;
                    }

                    avFrame->pts += mDeltaPts;
                }

                auto *tmp = dynamic_cast<AVAFFrame *>(frame);

                if (tmp) {
                    tmp->updateInfo();
                }

#ifdef DUMP_PCM
                // int planar = av_sample_fmt_is_planar((enum AVSampleFormat) filt_frame->format);
                // int channels = av_get_channel_layout_nb_channels(filt_frame->channel_layout);
                // int planes = planar ? channels : 1;
                // int bps = av_get_bytes_per_sample((enum AVSampleFormat) filt_frame->format);
                // int plane_size = bps * filt_frame->nb_samples * (planar ? 1 : channels);
                // write(fd, filt_frame->extended_data[0], plane_size);
#endif
                frame->getInfo().timePosition = timePosition;
                frame->getInfo().utcTime = utcTime;
                mOutPut.push(frame);
                frame = nullptr;
            }
        }

        if (mActive) {
            af_msleep(10);
        }

        return 0;
    }

    int ffmpegAudioFilter::pull(unique_ptr<IAFFrame> &frame, uint64_t timeOut)
    {
        if (mOutPut.empty()) {
            return -EAGAIN;
        }

        frame = unique_ptr<IAFFrame>(mOutPut.front());
        mOutPut.pop();
        return 0;
    }

    void ffmpegAudioFilter::flush()
    {
        if (mPThread) {
            mPThread->pause();
        }

        while (!mPts.empty()) {
            mPts.pop();
        }

        while (!mInPut.empty()) {
            delete mInPut.front();
            mInPut.pop();
        }

        while (!mOutPut.empty()) {
            delete mOutPut.front();
            mOutPut.pop();
        }

        /*
         * flush 不只是清空 AVFilterGraph 的缓存，还必须清空“输入时间轴状态”。
         * DASH/HLS seek 或清晰度切换后，下一段音频的 PTS 会从新的 segment 起点
         * 重新开始；如果保留上一段的 mLastInputPts/mFirstPts/mDeltaPts，下面的
         * discontinuity 计算会把新旧时间轴差值累计进去。日志中的
         *   mDeltaPts is 50154667 / 87168001
         * 就是这个问题，随后音频时钟会整体领先/落后视频几十秒。
         */
        mFirstPts = INT64_MIN;
        mDeltaPts = 0;
        mLastInputPts = INT64_MIN;
        mLastInPutDuration = 0;

        if (m_pFilterGraph) {
            avfilter_graph_free(&m_pFilterGraph);
        }

        if (mPThread) {
            mPThread->start();
        }
    }

}
