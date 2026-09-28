//
// Created by moqi on 2019-07-08.
//
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/intreadwrite.h>
};
#define LOG_TAG "avFormatDemuxer"

#include <utils/frame_work_log.h>
#include "utils/ffmpeg_utils.h"
#include <utils/errors/framework_error.h>
#include <utils/AFMediaType.h>
#include "avFormatDemuxer.h"
#include "base/media/AVAFPacket.h"
#include "AVBSF.h"
#include <mutex>
#include <utils/CicadaUtils.h>
#include <cassert>
#include <utils/timer.h>
#include <utils/CicadaJSON.h>
#include "play_list/HlsParser.h"
#include "dash/MPDParser.h"

using namespace std;
namespace Cicada {
    static const int INITIAL_BUFFER_SIZE = 32768;

    avFormatDemuxer avFormatDemuxer::se(0);

    void avFormatDemuxer::init()
    {
        mName = LOG_TAG;
        mCtx = avformat_alloc_context();
        mCtx->interrupt_callback.callback = interrupt_cb;
        mCtx->interrupt_callback.opaque = this;
        // AVFMT_FLAG_KEEP_SIDE_DATA removed in FFmpeg 6.0（5.0 起即 no-op，
        // side data 总是保留）
#if AF_HAVE_PTHREAD
        mPthread = NEW_AF_THREAD(readLoop);
#endif
    }

    avFormatDemuxer::avFormatDemuxer()
    {
        init();
    }

    avFormatDemuxer::avFormatDemuxer(const string &path) : IDemuxer(path)
    {
        AF_TRACE;
        init();
    }

    int avFormatDemuxer::interrupt_cb(void *opaque)
    {
        return static_cast<avFormatDemuxer *>(opaque)->mInterrupted;
    }

    avFormatDemuxer::~avFormatDemuxer()
    {
        AF_TRACE;
        Close();
#if AF_HAVE_PTHREAD
        delete mPthread;
        mPthread = nullptr;
#endif
    }

    int avFormatDemuxer::Open()
    {
        return open(nullptr);
    }

    int avFormatDemuxer::open(const AVInputFormat *in_fmt)
    {
        if (bOpened) {
            return 0;
        }

        int64_t startTime = af_getsteady_ms();
        bool use_filename = false;

        if (mReadCb != nullptr ) {
            uint8_t *read_buffer = static_cast<uint8_t *>(av_malloc(INITIAL_BUFFER_SIZE));
            mPInPutPb = avio_alloc_context(read_buffer, INITIAL_BUFFER_SIZE, 0, this, mReadCb ? avio_callback_read : nullptr, nullptr,
                                           mSeekCb ? avio_callback_seek : nullptr);

            if (mPInPutPb == nullptr) {
                av_free(read_buffer);
                return -ENOMEM;
            }

            mCtx->pb = mPInPutPb;
        } else {
            use_filename = true;
        }

        av_dict_set_int(&mInputOpts, "safe", 0, 0);
        av_dict_set(&mInputOpts, "protocol_whitelist", "file,http,https,tcp,tls", 0);
        av_dict_set_int(&mInputOpts, "usetoc", 1, 0);
        av_dict_set_int(&mInputOpts, "flv_strict_header", 1, 0);
        /*If a url with mp4 ext name, but is not a mp4 file, the mp4 demuxer will be matched
         * by ext name , mp4 demuxer will try to find moov box, it will ignore the return value
         * of the avio_*, and don't check interrupt flag, if the url is a network file, here will
         * blocking. To avoid this, when the filename is not opened by ffmpeg, change the file name
         * to a fake name ,only tell ffmpeg this is not a local file, and ffmpeg will use it
         * in:
         * ff_configure_buffers_for_index()
         * this optimally the mov demuxing  when moov box at the end of the file.
         *
         * But if the filename is used for ffmpeg to open, it's can't do this.
         */
        const char *filename = mPath.c_str();

        if (!use_filename) {
            if (CicadaUtils::startWith(mPath, {"http://", "https://"})) {
                const AVInputFormat *mp4Format = av_find_input_format("mp4");

                if (mp4Format && av_match_ext(filename, mp4Format->extensions)) {
                    filename = "http://xxx";
                }
            }
        }

        //TODO: opts
        int ret = avformat_open_input(&mCtx, filename, in_fmt, mInputOpts ? &mInputOpts : nullptr);
        AVDictionaryEntry *t;

        if ((t = av_dict_get(mInputOpts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
            AF_LOGD("Option %s not found.\n", t->key);
            // return AVERROR_OPTION_NOT_FOUND;
        }

        if (ret < 0) {
            AF_LOGE("avformat_open_input error %d,%s,", ret, getErrorString(ret));

            if (ret == AVERROR_PROTOCOL_NOT_FOUND) {
                return FRAMEWORK_ERR_PROTOCOL_NOT_SUPPORT;
            }

            if (ret == AVERROR_EXIT || mInterrupted) {
                return FRAMEWORK_ERR_EXIT;
            }

            return ret;
        }

        int probeHeader_nbStreams = mCtx->nb_streams;
        int64_t probeHeader_pos = -1;
        int probeHeader_seekCount = -1;
        if (mCtx->pb != nullptr) {
            probeHeader_pos = mCtx->pb->bytes_read;
            // AVIOContext::seek_count removed in FFmpeg 6.0（仅探测统计，保持未知）
        }

        if (mSeekCb == nullptr && strcmp(mCtx->iformat->name, "mpegts") == 0) {
            mNedParserPkt = true;
        }

        mCtx->flags |= AVFMT_FLAG_GENPTS;
        /* normal mp4 already have fps after avformat_open_input, so probe fps will not cost additional time
         * special mp4 (https://ll-hls-test.apple.com/llhls1/media1/lowLatencyHLS.m3u8 some front packets don't have duration)
         * does not have fps after open, so need to set fps_probe_size to probe fps in avformat_find_stream_info. if not set, 
         * steam will not have fps after avformat_find_stream_info, and lead to wrong packet duration in av_read_frame */
        if (strcmp(mCtx->iformat->name, "mov,mp4,m4a,3gp,3g2,mj2") != 0) {
            mCtx->fps_probe_size = 0;
        } else {
            mCtx->fps_probe_size = 5;
        }
        // TODO: only find ts and flv's info?

        if (mMetaInfo) {
            for (int i = 0; i < mCtx->nb_streams; ++i) {
                if (i >= mMetaInfo->meta.size()) {
                    break;
                }

                set_stream_meta(mCtx->streams[i], (Stream_meta *) *mMetaInfo->meta[i].get());
            }

            // TODO: set it to zero can avoid read and decode more frames,but wen seek to back in hlsstream,will lead pts err
            if (mMetaInfo->bContinue) {
                mCtx->max_ts_probe = 0;
            }
        }

        ret = avformat_find_stream_info(mCtx, nullptr);

        if (mInterrupted) {
            AF_LOGD("interrupted\n");
            return FRAMEWORK_ERR_EXIT;
        }

        if (ret < 0 && ret != AVERROR_EOF) {
            AF_LOGE("avformat_find_stream_info error %d:%s\n", ret, getErrorString(ret));
            return ret;
        }

        int64_t probeStream_pos = -1;
        int probeStream_seekCount = -1;
        if (mCtx->pb != nullptr) {
            probeStream_pos = mCtx->pb->bytes_read;
            // AVIOContext::seek_count removed in FFmpeg 6.0（仅探测统计，保持未知）
        }

        int probeStream_nbFrames = 0;
        // AVFormatContext::nb_frames removed in FFmpeg 6.0；改为对
        // AVStream::nb_frames（仍为公共字段）求和（仅探测统计）
        for (unsigned int i = 0; i < mCtx->nb_streams; ++i) {
            probeStream_nbFrames += (int) mCtx->streams[i]->nb_frames;
        }

        /*
         * this flag is only affect on mp3 and flac
         */
        if (mCtx->duration > 600 * AV_TIME_BASE && strcmp(mCtx->iformat->name, "mp3") == 0) {
            mCtx->flags |= AVFMT_FLAG_FAST_SEEK;
        }

        bOpened = true;
        int64_t used = af_getsteady_ms() - startTime;
        CicadaJSONItem json;
        json.addValue("cost", (int) used);
        json.addValue("time", (double) af_getsteady_ms());
        json.addValue("headerPos" , (double) probeHeader_pos);
        json.addValue("headerSeekCount" , (int) probeHeader_seekCount);
        json.addValue("headerNbStreams" , (int) probeHeader_nbStreams);
        json.addValue("streamPos" , (double)probeStream_pos);
        json.addValue("streamSeekCount" , (int)probeStream_seekCount);
        json.addValue("streamNbFrames" , (int)probeStream_nbFrames);
        mProbeString = json.printJSON();

        if (mStartTime > 0 && mStartTime < mCtx->duration) {
            Seek(mStartTime, 0, -1);
        }

        return 0;
    }

    void avFormatDemuxer::Close()
    {
        Stop();

        if (mCtx) {
            avformat_close_input(&mCtx);
        }

        if (mPInPutPb) {
            av_free(mPInPutPb->buffer);
            av_free(mPInPutPb);
            mPInPutPb = nullptr;
        }

        mStreamCtxMap.clear();
        mPacketQueue.clear();
        bOpened = false;

        if (mInputOpts) {
            av_dict_free(&mInputOpts);
        }
    }

    int avFormatDemuxer::ReadPacketInternal(unique_ptr<IAFPacket> &packet)
    {
        if (!bOpened) {
            return -EINVAL;
        }

        AVPacket *pkt = av_packet_alloc();
        int err;

        do {
            err = av_read_frame(mCtx, pkt);

            if (err < 0) {
                if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
                    if (mCtx->pb && mCtx->pb->error != AVERROR_EXIT) {
                        av_log(NULL, AV_LOG_WARNING, "%s:%d: %s, ctx->pb->error=%d\n", __FILE__, __LINE__, getErrorString(err),
                               mCtx->pb->error);
                    }
                }

                if (mCtx->pb && mCtx->pb->error == FRAMEWORK_ERR_EXIT) {
                    av_packet_free(&pkt);
                    return FRAMEWORK_ERR_EXIT;
                }

                if (err == AVERROR_EOF) {
                    if (mCtx->pb && mCtx->pb->error == AVERROR(EAGAIN)) {
                        av_packet_free(&pkt);
                        return mCtx->pb->error;
                    }

                    if (mCtx->pb && mCtx->pb->error < 0) {
                        av_packet_free(&pkt);
                        int ret = mCtx->pb->error;
                        mCtx->pb->error = 0;
                        return ret;
                    }

                    av_packet_free(&pkt);
                    return 0;// EOS
                }

                if (err == AVERROR_EXIT) {
                    AF_LOGE("AVERROR_EXIT\n");
                    av_packet_free(&pkt);
                    return -EAGAIN;
                }

                if (err == AVERROR(EAGAIN) || err == SEGEND) {
                    if (mCtx->pb) {
                        mCtx->pb->eof_reached = 0;
                        mCtx->pb->error = 0;
                    }
                }

                av_packet_free(&pkt);
                return err;
            }

            if (mStreamCtxMap[pkt->stream_index] && mStreamCtxMap[pkt->stream_index]->opened) {
                break;
            }

            av_packet_unref(pkt);
        } while (true);

        // FFmpeg >= 5.9 computes packet fields inside av_read_frame() itself;
        // the vendored av_compute_pkt_fields workaround is only for old FFmpeg.
#if LIBAVFORMAT_VERSION_MAJOR < 59
        if (mNedParserPkt) {
            int old_duration = pkt->duration;
            av_compute_pkt_fields(mCtx, mCtx->streams[pkt->stream_index], nullptr, pkt, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
            assert(old_duration <= 0 || pkt->duration > 0);
        }
#endif

        if (pkt->pts == AV_NOPTS_VALUE) {
            AF_LOGW("pkt pts error\n");
        }

        if (pkt->dts == AV_NOPTS_VALUE) {
            AF_LOGW("pkt dts error\n");
        }

        int streamIndex = pkt->stream_index;

        /*
         * 【防御性形态回退】清单层说这条流不加密，于是 OpenStream 时就把 bsf 建好、
         * codecpar 也已经被改写成 Annex B 了；但万一这个判断是错的（包上真的带了
         * AV_PKT_DATA_ENCRYPTION_INFO），就必须当场把形态还原：撤掉 bsf，并把 codecpar
         * 的 extradata 恢复成 bsf 改写之前的容器原形态。这样包与 meta 同时回到容器原形态，
         * 与"从头就不建 bsf"的 DRM 路径完全一致，只撤 bsf 不还原 codecpar 会造成反向错位。
         *
         * 这是状态判断，不是重试或超时兜底：判据就是这一包自己的 side data。
         */
        if (mStreamCtxMap[streamIndex] != nullptr && mStreamCtxMap[streamIndex]->bsf != nullptr) {
            // FFmpeg 9: av_packet_get_side_data 的尺寸参数为 size_t*
            size_t encryption_info_size = 0;
            const uint8_t *new_encryption_info = av_packet_get_side_data(pkt, AV_PKT_DATA_ENCRYPTION_INFO, &encryption_info_size);

            if (encryption_info_size > 0 && new_encryption_info != nullptr) {
                dropBsf(streamIndex);
            }
        }

        // If is drm encrypted, should not create bsf.
        // But can not know if stream is encrypted when open stream,
        // so create bsf here when got first pkt.
        if (mStreamCtxMap[pkt->stream_index] != nullptr && !mStreamCtxMap[pkt->stream_index]->bsfInited) {
            createBsf(pkt, streamIndex);

            if (mStreamCtxMap[pkt->stream_index] != nullptr) {
                mStreamCtxMap[pkt->stream_index]->bsfInited = true;
            }
        }

        if (mStreamCtxMap[pkt->stream_index] == nullptr) {
            /*
             * createBsf 建 bsf 失败时会把流上下文置空，原有语义是"这条流不再投递包"
             * （与上面读循环里"上下文为空就丢弃这个包"的处理一致）。原代码在这里会直接
             * 解引用空指针，而"开流时就提前建 bsf"让这条失败路径更容易走到，
             * 所以显式收口成丢弃这一包。
             */
            av_packet_free(&pkt);
            return -EAGAIN;
        }

        bool needUpdateExtraData = false;
        // FFmpeg 9: av_packet_get_side_data 的尺寸参数为 size_t*
        size_t new_extradata_size = 0;
        const uint8_t *new_extradata = av_packet_get_side_data(pkt,
                                       AV_PKT_DATA_NEW_EXTRADATA,
                                       &new_extradata_size);

        if (new_extradata) {
            AF_LOGI("AV_PKT_DATA_NEW_EXTRADATA");
            AVCodecParameters *codecpar = mCtx->streams[streamIndex]->codecpar;
            av_free(codecpar->extradata);
            codecpar->extradata = static_cast<uint8_t *>(av_malloc(new_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
            memcpy(codecpar->extradata, new_extradata, new_extradata_size);
            codecpar->extradata_size = static_cast<int>(new_extradata_size);
            createBsf(pkt, streamIndex);
            needUpdateExtraData = true;
        }
        /*
         * TODO: can't support this for now, audio render only support fixed sample size
         */
        av_packet_shrink_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, 0);

        if (mStreamCtxMap[pkt->stream_index]->bsf) {
            // TODO: while pulling and ret value
            /*
             * pkt.stream_index will be reset to zero after push
             */
            // TODO: deal with error
            int index = pkt->stream_index;
            mStreamCtxMap[index]->bsf->push(pkt);
            int ret = mStreamCtxMap[index]->bsf->pull(pkt);

            if (ret < 0) {
                av_packet_free(&pkt);
                return -EAGAIN;
            }
        }

        int packet_size = pkt->size;

        if (pkt->pts != AV_NOPTS_VALUE) {
            pkt->pts = av_rescale_q(pkt->pts, mCtx->streams[pkt->stream_index]->time_base, av_get_time_base_q());
        }

        if (pkt->dts != AV_NOPTS_VALUE) {
            pkt->dts = av_rescale_q(pkt->dts, mCtx->streams[pkt->stream_index]->time_base, av_get_time_base_q());
        }

        if (pkt->duration > 0) {
            pkt->duration = av_rescale_q(pkt->duration, mCtx->streams[pkt->stream_index]->time_base,
                                         av_get_time_base_q());
        } else if (mCtx->streams[pkt->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            AVCodecParameters *codecpar = mCtx->streams[pkt->stream_index]->codecpar;
            if (codecpar->sample_rate > 0 && codecpar->frame_size > 0) {
                pkt->duration = codecpar->frame_size * 1000000 / codecpar->sample_rate;
            }
        }

        packet = unique_ptr<IAFPacket>(new AVAFPacket(&pkt, mSecretDemxuer));

        if (mSecretDemxuer){
            packet->setMagicKey(mDrmMagicKey);
        }

        if (needUpdateExtraData) {
            packet->setExtraData(mCtx->streams[streamIndex]->codecpar->extradata, mCtx->streams[streamIndex]->codecpar->extradata_size);
        }

        if (packet->getInfo().pts != INT64_MIN) {
            if (mCtx->start_time == INT64_MIN) {
                mCtx->start_time = packet->getInfo().pts;
            }
            packet->getInfo().timePosition = packet->getInfo().pts - mCtx->start_time;
        }

        return packet_size;
    }

    int avFormatDemuxer::OpenStream(int index)
    {
#if AF_HAVE_PTHREAD
        std::unique_lock<std::mutex> uLock(mMutex);
#endif

        if (index >= mCtx->nb_streams) {
            AF_LOGE("no such stream\n");
            return -EINVAL;
        }

        if (mStreamCtxMap[index] != nullptr) {
            mStreamCtxMap[index]->opened = true;
            return 0;
        }

        mStreamCtxMap[index] = unique_ptr<AVStreamCtx>(new AVStreamCtx());
        mStreamCtxMap[index]->opened = true;
        mStreamCtxMap[index]->bsfInited = false;

        /*
         * 【形态对齐的唯一落点】非加密流在这里就把 head 合并 bsf 建起来。
         *
         * 为什么必须落在"开流"这一刻、而不是等第一个包：bsf 的 init 会把 codecpar 的
         * extradata 从 hvcC/AVCC 改写成 Annex B（AVBSF::init 最后一步是
         * avcodec_parameters_copy(codecpar, par_out)），而 GetStreamMeta 给出的 extradata
         * 就是这份 codecpar（get_stream_meta 直接读 AVStream 的 codecpar->extradata）。
         * 若 bsf 拖到第一个包才建，那么"内层 demuxer 刚建好、还没产出任何包"的窗口里，
         * GetStreamMeta 给的仍是容器的 hvcC，而随后真正投递的包已经是 bsf 转出的 Annex B，
         * 解码器按 4 字节长度前缀去拆 Annex B 的包必然每包失败（ff_h2645_packet_split
         * 返回 -1094995529，日志刷 re-assert 与 Invalid data），一帧不出、画面冻死，
         * 只有 seek（重建解码器）才恢复。HLS 每个分片都重建一次内层 demuxer
         * （HLSStream::createDemuxer），长期停在这个窗口里，所以几乎必中；DASH 只在刚打开
         * 目标档 / reopen 后的那一小段输掉竞态，所以偶发。
         *
         * 在 OpenStream 就建好之后，这条流的 codecpar 从"还没有任何包"开始就是包的形态，
         * 此后任何时刻取 meta 都与包同形态。
         *
         * 加密流（mStreamEncrypted 为真）不走这里，保持第一个包上懒建的原行为：DRM 时
         * bsf 本就不该建，包与 meta 都是容器原形态。清单层判错的情况由
         * ReadPacketInternal 里的防御性回退兜住。
         */
        if (!mStreamEncrypted && createBsf(nullptr, index) == 0 && mStreamCtxMap[index] != nullptr) {
            mStreamCtxMap[index]->bsfInited = true;
        }

        if (mStreamCtxMap[index] == nullptr) {
            /*
             * createBsf 建 bsf 失败时会把流上下文置空（原有语义）。开流这条新路径上不能让
             * 这个副作用把一条已经打开的流变成"包全被丢弃"：把上下文补回来，形态判定于是
             * 退回"第一个包上懒建"的原行为，等于这次提前建 bsf 没发生过。
             */
            mStreamCtxMap[index] = unique_ptr<AVStreamCtx>(new AVStreamCtx());
            mStreamCtxMap[index]->opened = true;
            mStreamCtxMap[index]->bsfInited = false;
        }

        return 0;
    }

    void avFormatDemuxer::CloseStream(int index)
    {
#if AF_HAVE_PTHREAD
        std::unique_lock<std::mutex> uLock(mMutex);
#endif

        if (mStreamCtxMap.find(index) == mStreamCtxMap.end()) {
            AF_LOGI("not opened\n");
            return;
        }

        mStreamCtxMap[index]->opened = false;
    }

    int avFormatDemuxer::createBsf(AVPacket *pkt, int index)
    {
        /*
         * pkt 为 nullptr 表示"开流前的提前创建"（见 OpenStream）：那时还没有包，
         * 也就无从判断加密，加密与否由清单层的 setStreamEncrypted 提示决定，
         * 这里只跳过"看包上的加密 side data"这一步。
         */
        if (pkt != nullptr) {
            // FFmpeg 9: av_packet_get_side_data 的尺寸参数为 size_t*
            size_t encryption_info_size = 0;
            const uint8_t *new_encryption_info = av_packet_get_side_data(pkt, AV_PKT_DATA_ENCRYPTION_INFO, &encryption_info_size);
            if (encryption_info_size > 0 && new_encryption_info != nullptr) {
                return 0;
            }
        }

        string bsfName{};
        int ret = 0;
        const AVCodecParameters *codecpar = mCtx->streams[index]->codecpar;

        if (mMergeVideoHeader == header_type::header_type_merge) {
            if (codecpar->codec_id == AV_CODEC_ID_H264 && codecpar->extradata != nullptr && (codecpar->extradata[0] == 1)) {
                bsfName = "h264_mp4toannexb";
            } else if (codecpar->codec_id == AV_CODEC_ID_HEVC && codecpar->extradata_size >= 5 &&
                       AV_RB32(codecpar->extradata) != 0x0000001 &&
                       AV_RB24(codecpar->extradata) != 0x000001) {
                bsfName = "hevc_mp4toannexb";
            }

            // TODO: mpeg4 dump extra bsf
        } else if (mMergeVideoHeader == header_type::header_type_extract) {
            if (codecpar->codec_id == AV_CODEC_ID_H264 && codecpar->extradata != nullptr && (codecpar->extradata[0] != 1)) {
                bsfName = "h26xAnnexb2xVcc";
            } else if (codecpar->codec_id == AV_CODEC_ID_HEVC && codecpar->extradata_size >= 5 &&
                       !(AV_RB32(codecpar->extradata) != 0x0000001 &&
                         AV_RB24(codecpar->extradata) != 0x000001)) {
                bsfName = "h26xAnnexb2xVcc";
            }
        }else if (mMergeVideoHeader == header_type::header_type_no_touch) {

        }

        if (!bsfName.empty()) {
#if AF_HAVE_PTHREAD
            std::lock_guard<std::mutex> uLock(mCtxMutex);
#endif
            /*
             * init 会把 codecpar 改写成 bsf 的输出形态，先留一份改写前的 extradata，
             * 供"提前建了 bsf 之后才发现是加密流"时还原（见 dropBsf）。
             */
            saveCodecParBeforeBsf(index);
            mStreamCtxMap[index]->bsf = unique_ptr<IAVBSF>(IAVBSFFactory::create(bsfName));
            ret = mStreamCtxMap[index]->bsf->init(bsfName, mCtx->streams[index]->codecpar);

            if (ret < 0) {
                AF_LOGE("create %s bsf error\n", bsfName.c_str());
                mStreamCtxMap[index] = nullptr;
            }
        }

        return ret;
    }

    void avFormatDemuxer::saveCodecParBeforeBsf(int index)
    {
        if (mCtx == nullptr || index < 0 || index >= mCtx->nb_streams) {
            return;
        }

        if (mStreamCtxMap.find(index) == mStreamCtxMap.end() || mStreamCtxMap[index] == nullptr) {
            return;
        }

        AVCodecParameters *codecpar = mCtx->streams[index]->codecpar;
        std::vector<uint8_t> &saved = mStreamCtxMap[index]->preBsfExtraData;
        saved.clear();

        if (codecpar->extradata != nullptr && codecpar->extradata_size > 0) {
            saved.assign(codecpar->extradata, codecpar->extradata + codecpar->extradata_size);
        }
    }

    void avFormatDemuxer::dropBsf(int index)
    {
        if (mCtx == nullptr || index < 0 || index >= mCtx->nb_streams) {
            return;
        }

#if AF_HAVE_PTHREAD
        std::lock_guard<std::mutex> uLock(mCtxMutex);
#endif
        /*
         * 上下文的存在性判断与使用都在锁内完成：createBsf 建 bsf 失败时会把上下文置空，
         * 那是在同一把锁里做的，锁外先判断再进来用会读到中途被置空的上下文。
         */
        if (mStreamCtxMap.find(index) == mStreamCtxMap.end() || mStreamCtxMap[index] == nullptr) {
            return;
        }

        AVStreamCtx *ctx = mStreamCtxMap[index].get();

        if (ctx->bsf == nullptr) {
            return;
        }

        AF_LOGW("stream %d got an encrypted packet, drop the header merge bsf and restore codecpar\n", index);
        ctx->bsf = nullptr;

        /*
         * 还原 codecpar 的 extradata：bsf 的 init 已经把它改写成了 Annex B，
         * 只撤 bsf 不还原的话，包（容器原形态）与 meta（Annex B）仍然错位，
         * 正好是本次要修的那个 bug 的镜像。
         */
        AVCodecParameters *codecpar = mCtx->streams[index]->codecpar;
        std::vector<uint8_t> &saved = ctx->preBsfExtraData;
        av_free(codecpar->extradata);
        codecpar->extradata = nullptr;
        codecpar->extradata_size = 0;

        if (!saved.empty()) {
            codecpar->extradata = static_cast<uint8_t *>(av_malloc(saved.size() + AV_INPUT_BUFFER_PADDING_SIZE));

            if (codecpar->extradata != nullptr) {
                memcpy(codecpar->extradata, saved.data(), saved.size());
                memset(codecpar->extradata + saved.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
                codecpar->extradata_size = static_cast<int>(saved.size());
            }
        }

        saved.clear();
    }

    void avFormatDemuxer::flush()
    {
        avio_flush(mCtx->pb);
        avformat_flush(mCtx);
        mError = 0;
    }

    void avFormatDemuxer::interrupt(int inter)
    {
        mInterrupted = inter;
    }

    int64_t avFormatDemuxer::Seek(int64_t us, int flags, int index)
    {
        us = getWorkAroundSeekPos(us);
        if (!bOpened) {
            mStartTime = us;
            return static_cast<int>(us);
        }

        if (mInterruptCb) {
            mInterruptCb(mUserArg, 1);
        }

        bPaused = true;
#if AF_HAVE_PTHREAD
        {
            std::unique_lock<std::mutex> waitLock(mQueLock);
            bPaused = true;
        }
        mQueCond.notify_one();
        mPthread->pause();
#else
        bPaused = true;
#endif

        if (mInterruptCb) {
            mInterruptCb(mUserArg, 0);
        }
        if(mCtx->pb->error < 0) {
            mCtx->pb->error = 0;
            avio_feof(mCtx->pb);
        }

        mPacketQueue.clear();
        mError = 0;
        if (mCtx->start_time == INT64_MIN) {
            mCtx->start_time = 0;
        }
        static const int jitter = 2;
        int64_t timestamp = mCtx->start_time + us;
        int64_t timestamp_seek;
        int ret;

        if (!flags) { //prev
            timestamp_seek = timestamp + AV_TIME_BASE * jitter;
            ret = avformat_seek_file(mCtx, -1, INT64_MIN, timestamp, timestamp_seek, 0);
        } else {
            if (timestamp - AV_TIME_BASE * jitter > INT64_MIN) {
                timestamp_seek = timestamp - AV_TIME_BASE * jitter;
            } else {
                timestamp_seek = INT64_MIN;
            }

            ret = avformat_seek_file(mCtx, -1, timestamp_seek, timestamp, INT64_MAX, 0);
        }

        bEOS = false;
        bPaused = false;
#if AF_HAVE_PTHREAD

        if (mPthread->getStatus() == afThread::THREAD_STATUS_PAUSED) {
            mPthread->start();
        }

#endif
        return ret;
    }

    int avFormatDemuxer::GetNbStreams() const
    {
        return mCtx->nb_streams;
    }


    int avFormatDemuxer::GetSourceMeta(Source_meta **meta) const
    {
        return AVDictionary2SourceMeta(meta, mCtx->metadata);
    }

    int avFormatDemuxer::GetMediaMeta(Media_meta *mediaMeta) const
    {

        if (mediaMeta == nullptr) {
            return -1;
        }

#if AF_HAVE_PTHREAD
        std::lock_guard<std::mutex> uLock(mCtxMutex);
#endif
        mediaMeta->totalBitrate = mCtx->bit_rate;

        return 0;
    }

    int avFormatDemuxer::GetStreamMeta(Stream_meta *meta, int index, bool sub) const
    {
#if AF_HAVE_PTHREAD
        std::lock_guard<std::mutex> uLock(mCtxMutex);
#endif
        if (index < 0 || mCtx == nullptr || index >= mCtx->nb_streams) {
            return -EINVAL;
        }

        get_stream_meta(mCtx->streams[index], meta);

        if (mCtx->duration != AV_NOPTS_VALUE) {
            meta->duration = mCtx->duration;
        } else {
            meta->duration = 0;
        }

        meta->index = index;
        return 0;
    }

    void avFormatDemuxer::Start()
    {
#if AF_HAVE_PTHREAD
        bPaused = false;
        mPthread->start();
#endif
    }

    void avFormatDemuxer::Stop()
    {
#if AF_HAVE_PTHREAD
        {
            std::unique_lock<std::mutex> waitLock(mQueLock);
            bPaused = true;
        }
        mQueCond.notify_one();

        if (mPthread) {
            mPthread->stop();
        }

#endif
    }

#if AF_HAVE_PTHREAD

    int avFormatDemuxer::readLoop()
    {
        if (bExited) {
            return -1;
        }

        if (bPaused) {
            return 0;
        }

        if (bEOS) {
            std::unique_lock<std::mutex> waitLock(mQueLock);

            if (bEOS) {
                mQueCond.wait(waitLock, [this]() { return bPaused || mInterrupted || bExited; });
            }
        }

        if (bEOS || bPaused) {
            return 0;
        }

        unique_ptr<IAFPacket> pkt{};
        int ret = ReadPacketInternal(pkt);

        if (ret > 0) {
            std::unique_lock<std::mutex> waitLock(mQueLock);

            if (mPacketQueue.size() > MAX_QUEUE_SIZE) {
                mQueCond.wait(waitLock, [this]() { return mPacketQueue.size() <= MAX_QUEUE_SIZE || bPaused || mInterrupted || bExited; });
            }

            mPacketQueue.push_back(std::move(pkt));
        } else if (ret == 0) {
            bEOS = true;
        } else {
            if (ret != AVERROR(EAGAIN) && ret != FRAMEWORK_ERR_EXIT) {
                mError = ret;
            }

            std::unique_lock<std::mutex> waitLock(mQueLock);
            mQueCond.wait_for(waitLock, std::chrono::milliseconds(10), [this]() { return bPaused || mInterrupted || bExited; });
        }

        return 0;
    }

#endif

    int avFormatDemuxer::ReadPacket(std::unique_ptr<IAFPacket> &packet, int index)
    {
#if !AF_HAVE_PTHREAD
        return ReadPacketInternal(packet);
#else

        if (mPthread->getStatus() == afThread::THREAD_STATUS_IDLE) {
            return ReadPacketInternal(packet);
        } else {
            std::unique_lock<std::mutex> waitLock(mQueLock);

            if (!mPacketQueue.empty()) {
                packet = std::move(mPacketQueue.front());
                mPacketQueue.pop_front();
                mQueCond.notify_one();
                return static_cast<int>(packet->getSize());
            }

            if (bEOS) {
                return 0;
            }

            if (mError < 0) {
                return mError;
            }

            return -EAGAIN;
        }

#endif
    }

    const std::string avFormatDemuxer::GetProperty(int index, const string &key) const
    {
        if (key == "probeInfo") {
            return mProbeString;
        } else if (key == "containerName") {
            std::lock_guard<std::mutex> uLock(mCtxMutex);
            if (mCtx == nullptr) {
                return "N/A";
            } else {
                string formatName = mCtx->iformat->name;
                if (formatName.find("mp4") != string::npos) {
                    return "mp4";
                } else {
                    return formatName;
                }
            }
        }

        return "";
    }

    bool avFormatDemuxer::isRealTimeStream(int index)
    {
#if AF_HAVE_PTHREAD
        std::lock_guard<std::mutex> uLock(mCtxMutex);
#endif
        if (mCtx == nullptr) {
            return false;
        }
        bool isLive = (mCtx->duration == AV_NOPTS_VALUE || mCtx->duration == 0);
        bool isHls = false;
        bool isDash = false;
        if (mCtx->iformat) {
            isHls = (strcmp(mCtx->iformat->name, "hls,applehttp") == 0);
            isDash = (strcmp(mCtx->iformat->name, "dash") == 0);
        }
        return isLive && !(isHls || isDash);
    }

    bool avFormatDemuxer::is_supported(const string &uri, const uint8_t *buffer, int64_t size, int *type, const Cicada::DemuxerMeta *meta,
                                       const Cicada::options *opts)
    {
#ifdef ENABLE_HLS_DEMUXER

        if (HlsParser::probe(buffer, size) > 0) {
            return false;
        }

#endif
#ifdef ENABLE_DASH_DEMUXER
        if (Dash::MPDParser::probe(buffer, size) > 0) {
            return false;
        }
#endif

        unsigned char *pbBuffer = static_cast<unsigned char *>(av_malloc(size + AVPROBE_PADDING_SIZE));
        memcpy(pbBuffer, buffer, size);
        memset(pbBuffer + size, 0, AVPROBE_PADDING_SIZE);
        AVProbeData pd = {uri.c_str(), const_cast<unsigned char *>(pbBuffer), static_cast<int>(size)};
        int score = AVPROBE_SCORE_RETRY;
        const AVInputFormat *fmt = av_probe_input_format2(&pd, 1, &score);
        av_freep(&pbBuffer);

        if (fmt && (strcmp(fmt->name, "hls,applehttp") == 0 || strcmp(fmt->name, "webvtt") == 0 || strcmp(fmt->name, "srt") == 0 ||
                    strcmp(fmt->name, "ass") == 0)) {
            return false;
        }

        // TODO: use more and more data to probe
        if (fmt || 1) {
            *type = demuxer_type_bit_stream;
            return true;
        }

        return false;
    }

    avFormatDemuxer::avFormatDemuxer(int dummy)
    {
        addPrototype(this);
        ffmpeg_init();
    }

    void avFormatDemuxer::PreStop()
    {
#if AF_HAVE_PTHREAD
        std::unique_lock<std::mutex> waitLock(mQueLock);
        bExited = true;
        mQueCond.notify_one();
#endif
    }
    int avFormatDemuxer::avio_callback_read(void *arg, uint8_t *buffer, int size)
    {
        auto *demuxer = static_cast<avFormatDemuxer *>(arg);
        int ret = demuxer->mReadCb(demuxer->mUserArg, buffer, size);
        return ret ? ret : AVERROR_EOF;
    }
    int64_t avFormatDemuxer::avio_callback_seek(void *arg, int64_t offset, int whence)
    {
        auto *demuxer = static_cast<avFormatDemuxer *>(arg);
        return demuxer->mSeekCb(demuxer->mUserArg, offset, whence);
    }
    int64_t avFormatDemuxer::getWorkAroundSeekPos(int64_t pos)
    {
        if (!bOpened || mCtx == nullptr || !(mCtx->flags & AVFMT_FLAG_FAST_SEEK) || mCtx->duration <= 0) {
            return pos;
        }
        return pos >= mCtx->duration - 2 * AV_TIME_BASE ? mCtx->duration - 2 * AV_TIME_BASE : pos;
    }
    bool avFormatDemuxer::isTSDiscontinue()
    {
        if (mCtx) {
            return (mCtx->iformat->flags & AVFMT_TS_DISCONT);
        }
        return true;
    }
    const vector<IDemuxer::streamIndexEntryInfo> &avFormatDemuxer::getStreamIndexEntryInfo()
    {
        if (mCtx == nullptr || !mEntryInfos.empty()) {
            return mEntryInfos;
        }
        for (int i = 0; i < mCtx->nb_streams; ++i) {
            streamIndexEntryInfo entryInfo;
            entryInfo.mDuration = mCtx->duration;
            switch (mCtx->streams[i]->codecpar->codec_type) {
                case AVMEDIA_TYPE_VIDEO:
                    entryInfo.type = STREAM_TYPE_VIDEO;
                    break;
                case AVMEDIA_TYPE_AUDIO:
                    entryInfo.type = STREAM_TYPE_AUDIO;
                    break;
                case AVMEDIA_TYPE_SUBTITLE:
                    entryInfo.type = STREAM_TYPE_SUB;
                    break;
                default:
                    break;
            }
            // AVStream::index_entries/nb_index_entries removed in FFmpeg 6.0；
            // 改用公共访问器 avformat_index_get_entries_count/get_entry
            int nbIndexEntries = avformat_index_get_entries_count(mCtx->streams[i]);
            for (int j = 0; j < nbIndexEntries; ++j) {
                const AVIndexEntry *index_entry = avformat_index_get_entry(mCtx->streams[i], j);
                int64_t timestamp = av_rescale_q(index_entry->timestamp, mCtx->streams[i]->time_base, av_get_time_base_q());
                streamIndexEntryInfo::entryInfo info(index_entry->pos, timestamp, index_entry->flags & AVINDEX_KEYFRAME,
                                                     index_entry->flags & AVINDEX_DISCARD_FRAME, index_entry->size);
                entryInfo.mEntry.push_back(info);
            }
            mEntryInfos.push_back(entryInfo);
        }
        //        int index = 0;
        //        for (const auto& item:mEntryInfos) {
        //            AF_LOGD("stream %d %d duration %lld\n",index,item.type,item.mDuration);
        //
        //            for (const auto& item1: mEntryInfos[index].mEntry) {
        //                AF_LOGD("time %lld pos %lld\n",item1.mTimestamp,item1.mPos);
        //            }
        //            index++;
        //        }
        return mEntryInfos;
    }

    int64_t avFormatDemuxer::getBufferDuration(int index) const
    {
        if (mGetBufferDuration) {
            return mGetBufferDuration(mUserArg, 0);
        }
        return 0;
    }

}
