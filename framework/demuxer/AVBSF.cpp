//
// Created by moqi on 2019-07-08.
//
#define LOG_TAG "AVBSF"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avc.h>
#include <libavformat/hevc.h>
#include <libavutil/intreadwrite.h>
}

#include <utils/frame_work_log.h>
#include "AVBSF.h"
#include "utils/ffmpeg_utils.h"
#include "AdtsBSF.h"

namespace Cicada {
    AVBSF::AVBSF()
    {
    }

    AVBSF::~AVBSF()
    {
        av_bsf_free(&bsfContext);
    }

    int AVBSF::init(const std::string &name, AVCodecParameters *codecpar)
    {
        const AVBitStreamFilter *bsf = av_bsf_get_by_name(name.c_str());

        if (!bsf) {
            AF_LOGE("%s bsf not found\n", name.c_str());
            return -EPERM;
        }

        int ret = av_bsf_alloc(bsf, &bsfContext);

        if (ret < 0) {
            AF_LOGE("Cannot alloc BSF!\n");
            return -ENOMEM;
        }

        avcodec_parameters_copy(bsfContext->par_in, codecpar);
        ret = av_bsf_init(bsfContext);

        if (ret < 0) {
            AF_LOGE("Error initializing bitstream filter: %s\n", bsf->name);
            return ret;
        }

        ret = avcodec_parameters_copy(codecpar, bsfContext->par_out);

        if (ret < 0) {
            return ret;
        }

        return 0;
    }

    int AVBSF::push(AVPacket *pkt)
    {
        int ret = av_bsf_send_packet(bsfContext, pkt);

        if (ret < 0) {
            AF_LOGE("av_bsf_send_packet error %d (%s)\n", ret,  getErrorString(ret));
        }

        return ret;
    }

    int AVBSF::pull(AVPacket *pkt)
    {
        int ret = av_bsf_receive_packet(bsfContext, pkt);

        if (ret == AVERROR_EOF) {
            return 0;
        } else if (ret < 0) {
            AF_LOGE("av_bsf_receive_packet error %d (%s)\n", ret,  getErrorString(ret));
            return ret;
        } else {
            return pkt->size;
        }
    }

    AFAVBSF::AFAVBSF()
    {
        mPkt = av_packet_alloc();
    }

    AFAVBSF::~AFAVBSF()
    {
        av_packet_free(&mPkt);
    }

    int AFAVBSF::init(const std::string &name, AVCodecParameters *codecpar)
    {
        if (name != "h26xAnnexb2xVcc") {
            return -EINVAL;
        }

        if (codecpar->codec_id != AV_CODEC_ID_H264 && codecpar->codec_id != AV_CODEC_ID_HEVC) {
            return -EINVAL;
        }

        mBNeedParser = updateH26xHeader2xxc(codecpar);
        mCodecId = codecpar->codec_id;
        return 0;
    }

    int AFAVBSF::push(AVPacket *pkt)
    {
        if (pkt == nullptr) {
            bEof = true;
            return 0;
        }

        if (bEof) {
            return -EINVAL;
        }

        if (mPkt->data || mPkt->side_data_elems) {
            return -EAGAIN;
        }

        av_packet_move_ref(mPkt, pkt);
        return 0;
    }

    // FFmpeg 9.0 移除了内部函数 ff_avc_parse_nal_units_buf
    // （libavformat/avc.h 已无声明），此处实现等价转换：
    // 剥离 AnnexB start code，每个 NAL 前写 4 字节大端长度（AVCC）。
    // 输出缓冲用 av_realloc 分配，以便 av_packet_from_data 直接接管。
    static int avc_annexb_to_mp4(const uint8_t *buf_in, int size, uint8_t **buf_out, int *size_out)
    {
        const uint8_t *p = buf_in;
        const uint8_t *end = buf_in + size;
        uint8_t *dst = nullptr;
        int dst_size = 0;

        *buf_out = nullptr;
        *size_out = 0;

        if (buf_in == nullptr || size <= 0) {
            return -EINVAL;
        }

        while (p < end) {
            const uint8_t *nal_start = nullptr;

            if (end - p >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
                nal_start = p + 4;
            } else if (end - p >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) {
                nal_start = p + 3;
            } else {
                // start code 前的游离零字节：跳过（与旧实现行为一致）
                p++;
                continue;
            }

            // 找到下一个 start code（00 00 01 或 00 00 00 01）
            const uint8_t *nal_end = nal_start;
            while (nal_end + 3 <= end) {
                if (nal_end[0] == 0 && nal_end[1] == 0 &&
                    (nal_end[2] == 1 ||
                     (nal_end + 3 < end && nal_end[2] == 0 && nal_end[3] == 1))) {
                    break;
                }
                nal_end++;
            }
            // 剥离 NAL 尾部属于下一个 start code 前导的零字节
            while (nal_end > nal_start && nal_end[-1] == 0) {
                nal_end--;
            }

            int nal_len = (int) (nal_end - nal_start);
            uint8_t *new_dst = (uint8_t *) av_realloc(dst, dst_size + 4 + nal_len);
            if (new_dst == nullptr) {
                av_free(dst);
                return AVERROR(ENOMEM);
            }
            dst = new_dst;
            AV_WB32(dst + dst_size, nal_len);
            memcpy(dst + dst_size + 4, nal_start, nal_len);
            dst_size += 4 + nal_len;

            p = nal_end;
        }

        if (dst == nullptr) {
            return -EINVAL;
        }
        *buf_out = dst;
        *size_out = dst_size;
        return 0;
    }

    int AFAVBSF::pull(AVPacket *pkt)
    {
        AVPacket *in = nullptr;
        int ret = get_packet(&in);

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                return 0;
            }

            return ret;
        }

        if (!mBNeedParser) {
            av_packet_move_ref(pkt, in);
            return pkt->size;
        }

        uint8_t *reformatted_data = nullptr;
        int size = in->size;

        if (mCodecId == AV_CODEC_ID_H264) {
            // FFmpeg 9.0 移除内部函数 ff_avc_parse_nal_units_buf，
            // 改用本地等价实现（见 avc_annexb_to_mp4）
            ret = avc_annexb_to_mp4(in->data, in->size, &reformatted_data, &size);
            if (ret < 0) {
                AF_LOGE("avc annexb to mp4 fail %d\n", ret);
                av_packet_free(&in);
                return ret;
            }
        } else if (mCodecId == AV_CODEC_ID_HEVC) {
            ff_hevc_annexb2mp4_buf(in->data, &reformatted_data, &size, 0, nullptr);
        } else {
            AF_LOGE("error codec id\n");
            av_packet_free(&in);
            return -EINVAL;
        }

        if (reformatted_data) {
            av_packet_from_data(pkt, reformatted_data, size);
            av_packet_copy_props(pkt, in);
            av_packet_free(&in);
            return pkt->size;
        }

        av_packet_free(&in);
        return -EINVAL;
    }

    int AFAVBSF::get_packet(AVPacket **pkt)
    {
        if (bEof) {
            return 0;
        }

        if (!mPkt->data && !mPkt->side_data_elems) {
            return AVERROR(EAGAIN);
        }

        AVPacket *tmp_pkt;
        tmp_pkt = av_packet_alloc();

        if (!tmp_pkt) {
            return AVERROR(ENOMEM);
        }

        *pkt = mPkt;
        mPkt = tmp_pkt;
        return 0;
    }

    IAVBSF *IAVBSFFactory::create(const std::string &name)
    {
        if (name == "h26xAnnexb2xVcc") {
            return new AFAVBSF();
        }else if(name == "latm2Adts") {
            return new AdtsBSF();
        }

        return new AVBSF();
    }
}
