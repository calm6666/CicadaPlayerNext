//
// Created by moqi on 2019/11/7.
//

#ifndef CICADAPLAYERSDK_SAMPLEDECRYPTDEMUXER_H
#define CICADAPLAYERSDK_SAMPLEDECRYPTDEMUXER_H

#include "ISampleDecryptor.h"
#include "ISampleDecrypt2c.h"
#include <demuxer/avFormatDemuxer.h>

namespace Cicada {
    /**
     * Sample-level decryption wrapper.
     *
     * FFmpeg 6.1+ removed the ability to register custom AVInputFormat
     * demuxers with callbacks (read_header/read_packet moved to the internal
     * FFInputFormat), so this wrapper no longer uses a custom demuxer: it
     * demuxes with the standard avFormatDemuxer and decrypts each packet
     * in-place after ReadPacket().
     */
    class SampleDecryptDemuxer : public avFormatDemuxer {

    public:
        explicit SampleDecryptDemuxer();

        void setDecryptor(ISampleDecryptor *decryptor);

        int Open() override;

        int ReadPacket(std::unique_ptr<IAFPacket> &packet, int index) override;

    private:

        explicit SampleDecryptDemuxer(int dummy);

        Cicada::IDemuxer *clone(const string &uri, int type, const Cicada::DemuxerMeta *meta) override
        {
            return new SampleDecryptDemuxer();
        }

        bool is_supported(const string &uri, const uint8_t *buffer, int64_t size, int *type, const Cicada::DemuxerMeta *meta,
                          const Cicada::options *opts) override
        {
            return false;
        }

        static SampleDecryptDemuxer se;

    private:
        string mKey = "";
        int mCircleCount = 10;

    private:
        ISampleDecryptor *mDecryptor = nullptr;
    };
}


#endif //CICADAPLAYERSDK_SAMPLEDECRYPTDEMUXER_H
