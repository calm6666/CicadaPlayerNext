//
// Created by moqi on 2019/11/7.
//

#define LOG_TAG "SampleDecryptDemuxer"

#include "SampleDecryptDemuxer.h"
#include <base/media/AVAFPacket.h>
#include <utils/frame_work_log.h>

namespace Cicada {
    SampleDecryptDemuxer SampleDecryptDemuxer::se(0);

    SampleDecryptDemuxer::SampleDecryptDemuxer()
    {
    }

    void SampleDecryptDemuxer::setDecryptor(ISampleDecryptor *decryptor)
    {
        mDecryptor = decryptor;
    }

    int SampleDecryptDemuxer::Open()
    {
        if (mDecryptor == nullptr) {
            return -EINVAL;
        }
        // FFmpeg 6.1+ removed custom AVInputFormat registration: demux with the
        // standard avFormatDemuxer and decrypt packets in ReadPacket() below.
        return avFormatDemuxer::Open();
    }

    int SampleDecryptDemuxer::ReadPacket(std::unique_ptr<IAFPacket> &packet, int index)
    {
        int ret = avFormatDemuxer::ReadPacket(packet, index);

        if (ret > 0 && mDecryptor != nullptr && packet != nullptr) {
            auto *avafPacket = dynamic_cast<AVAFPacket *>(packet.get());
            if (avafPacket != nullptr) {
                AVPacket *pkt = avafPacket->ToAVPacket();
                if (pkt != nullptr && pkt->stream_index >= 0 && pkt->stream_index < mCtx->nb_streams) {
                    int size = SampleDecryptDec(mDecryptor,
                                                mCtx->streams[pkt->stream_index]->codecpar->codec_id,
                                                pkt->data, pkt->size);
                    if (size > 0) {
                        pkt->size = size;
                        ret = size;
                    } else {
                        AF_LOGE("SampleDecryptDec error\n");
                        ret = -EINVAL;
                    }
                }
            }
        }

        return ret;
    }

    SampleDecryptDemuxer::SampleDecryptDemuxer(int dummy) : avFormatDemuxer(dummy)
    {
        // FFmpeg 6.1+ removed av_register_input_format; the custom demuxer
        // (sampleDecryptDec.c) is no longer used — decryption happens in
        // ReadPacket() instead.
    }
}
