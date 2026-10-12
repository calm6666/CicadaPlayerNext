//
// Created by moqi on 2018/10/30.
//

#ifndef CICADA_PLAYER_FFMPEGAUDIOFILTER_H
#define CICADA_PLAYER_FFMPEGAUDIOFILTER_H
extern "C" {
#include <libavfilter/avfilter.h>
};

#include "IAudioFilter.h"
#include <utils/afThread.h>
#include <queue>
#include <boost/lockfree/spsc_queue.hpp>

namespace Cicada {
    class ffmpegAudioFilter : public IAudioFilter {
    public:
        ffmpegAudioFilter(const format &srcFormat, const format &dstFormat, bool active);

        ~ffmpegAudioFilter() override;

        static bool isSupported(string capacity);

        bool setOption(const string &key, const string &value, const string &capacity) override;

        int init(uint64_t flags) override;


        int push(std::unique_ptr<IAFFrame> &frame, uint64_t timeOut) override;

        int pull(unique_ptr<IAFFrame> &frame, uint64_t timeOut) override;

        void flush() override;


    private:
        int FilterLoop();

        int createSrcBufferFilter();

        AVFilterContext *createFilter(const char *name, const char *opt);

    private:
        typedef boost::lockfree::spsc_queue<IAFFrame *, boost::lockfree::capacity<20>> spsc_queue;
        std::atomic<double> mRate{1.0};
        double mVolume = 1.0;
        /*
         * 【音量均衡】当前响度压缩档位：0 关闭 / 1 标准 / 2 高动态
         * （docs/audio-loudness-modes.md §5）。init() 重建图时按它构建对应档参数，
         * 运行中切换走 applyLoudnessToGraph() 热更新（不重建）。
         * atomic：SetLoudness 可从任意线程写（setOption），FilterLoop 线程 init/读——
         * 与 mRate 同一读写模式；跨线程改图本身由 mMutexRate 保护（见 setOption）。
         */
        std::atomic<int> mLoudnessMode{0};
        AVFilterGraph *m_pFilterGraph{};
        afThread *mPThread{nullptr};

        AVFilterContext *mAbuffer_ctx{};
        AVFilterContext *mAbuffersink_ctx{};
        spsc_queue mInPut{};
        spsc_queue mOutPut{};
        boost::lockfree::spsc_queue<int64_t, boost::lockfree::capacity<10>> mPts{};
        std::mutex mMutexRate{};
        int64_t mFirstPts = INT64_MIN;
        int64_t mDeltaPts = 0;

        std::atomic<int64_t> mLastInputPts {INT64_MIN};
        std::atomic<int64_t> mLastInPutDuration {0};

        uint64_t mFlags{0};

        int addFilter(AVFilterContext **current, const char *name, const char *options_str);

        /* 【音量均衡】把 mLoudnessMode 对应档位的参数逐条热更新到已建图的 acompressor
           （调用方必须已持有 mMutexRate —— avfilter_graph_send_command 与 FilterLoop
            并发访问图不是线程安全的，见 atempo 分支的同一先例） */
        void applyLoudnessToGraph();
    };
}


#endif //CICADA_PLAYER_FFMPEGAUDIOFILTER_H
