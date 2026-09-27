/*
 * Copyright (c) 2012 pingkai010@gmail.com
 *
 *
 */
#ifndef CICADA_TYPE_H
#define CICADA_TYPE_H

#include <stdint.h>

typedef enum {
    DEMUX_MODE_NORMOL,
    DEMUX_MODE_I_FRAME,
} Demux_mode_e;

typedef enum {
    STREAM_TYPE_UNKNOWN = -1,
    STREAM_TYPE_VIDEO,
    STREAM_TYPE_AUDIO,
    STREAM_TYPE_SUB,
    STREAM_TYPE_MIXED,
    STREAM_TYPE_NUM,
} Stream_type;

enum AFCodecID {
    AF_CODEC_ID_NONE,

    AF_CODEC_ID_H264,
    AF_CODEC_ID_MPEG4,
    //    AF_CODEC_ID_RV30,
    //    AF_CODEC_ID_RV40,
    //    AF_CODEC_ID_MPEG2VIDEO,
    //    AF_CODEC_ID_VC1,
    //    AF_CODEC_ID_WMV3,
    //    AF_CODEC_ID_WMV1,
    //    AF_CODEC_ID_WMV2,
    //    AF_CODEC_ID_MSMPEG4V2,
    //    AF_CODEC_ID_DIV311,
    //    AF_CODEC_ID_FLV1,
    //    AF_CODEC_ID_SVQ3,
    //    AF_CODEC_ID_MPEG1VIDEO,
    //    AF_CODEC_ID_VP6,
    AF_CODEC_ID_VP8,
    AF_CODEC_ID_VP9,
    //    AF_CODEC_ID_MJPEG,
    //    AF_CODEC_ID_H263,
    AF_CODEC_ID_AV1,
    AF_CODEC_ID_HEVC,

    AF_CODEC_ID_AAC,
    AF_CODEC_ID_AC3,
    AF_CODEC_ID_EAC3,
    //    AF_CODEC_ID_DTS,
    //    AF_CODEC_ID_DTSE,
    AF_CODEC_ID_MP3,
    //    AF_CODEC_ID_APE,
    //    AF_CODEC_ID_COOK,
    //    AF_CODEC_ID_SIPR,
    //    AF_CODEC_ID_QDM2,
    AF_CODEC_ID_MP2,
    AF_CODEC_ID_MP1,
    AF_CODEC_ID_OPUS,
    //    AF_CODEC_ID_AMR_NB,
    //    AF_CODEC_ID_WMAV2,
    //    AF_CODEC_ID_WMAPRO,
    AF_CODEC_ID_PCM_S16LE,
    AF_CODEC_ID_PCM_S16BE,
    //    AF_CODEC_ID_PCM_BLURAY,
    //    AF_CODEC_ID_ADPCM,
    //    AF_CODEC_ID_PCM_S24LE,
    AF_CODEC_ID_PCM_U8,
    //    AF_CODEC_ID_PCM_MULAW,
    //    AF_CODEC_ID_ATRAC3,
    //    AF_CODEC_ID_VORBIS,
    //    AF_CODEC_ID_ALAC,
    //    AF_CODEC_ID_FLAC,

    AF_CODEC_ID_WEBVTT,
    //    AF_CODEC_ID_TEXT,
    AF_CODEC_ID_ASS,
    //    AF_CODEC_ID_SRT,

    /*
     * Audio codecs that the FFmpeg build has always contained but that the
     * framework had no id for. They are appended at the END on purpose: the
     * places they logically belong are still commented out above, but
     * uncommenting one of those would shift every later enumerator, and this
     * value travels inside Stream_meta. Appending keeps every existing id
     * stable.
     *
     * Each id needs three coordinated edits, otherwise the codec is compiled
     * into FFmpeg but the framework never asks for it and playback fails with
     * codec_error_audio_not_support (-513):
     *   1. here,
     *   2. a case in AVCodecID2CodecID  (ffmpeg_utils.c)
     *   3. a row in codec_pair_table    (ffmpeg_utils.c)
     *
     * FLAC in an MKV is the case that surfaced this: the demuxer reported
     * AV_CODEC_ID_FLAC, the reverse map had no case for it, the stream came out
     * as AF_CODEC_ID_NONE, and avcodec_find_decoder() was handed
     * AV_CODEC_ID_NONE.
     */
    AF_CODEC_ID_FLAC,
    AF_CODEC_ID_VORBIS,
    AF_CODEC_ID_ALAC,
    AF_CODEC_ID_DCA,
    AF_CODEC_ID_APE,
    AF_CODEC_ID_WAVPACK,
    AF_CODEC_ID_TTA,
    AF_CODEC_ID_WMAV1,
    AF_CODEC_ID_WMAV2,
    AF_CODEC_ID_WMAPRO,
    AF_CODEC_ID_TRUEHD,
    AF_CODEC_ID_MLP,
    AF_CODEC_ID_SPEEX,
    AF_CODEC_ID_AMR_NB,
    AF_CODEC_ID_AMR_WB,
    AF_CODEC_ID_SBC,
    AF_CODEC_ID_ATRAC3,
    AF_CODEC_ID_COOK,
    AF_CODEC_ID_RA_144,
    AF_CODEC_ID_NELLYMOSER,
    AF_CODEC_ID_ADPCM,
    AF_CODEC_ID_PCM_S24LE,
    AF_CODEC_ID_PCM_S24BE,
    AF_CODEC_ID_PCM_S32LE,
    AF_CODEC_ID_PCM_S32BE,
    AF_CODEC_ID_PCM_F32LE,
    AF_CODEC_ID_PCM_F64LE,
    AF_CODEC_ID_PCM_S8,
    AF_CODEC_ID_PCM_MULAW,
    AF_CODEC_ID_PCM_ALAW,
};


enum AFSampleFormat {
    AF_SAMPLE_FMT_NONE = -1,
    AF_SAMPLE_FMT_U8,          ///< unsigned 8 bits
    AF_SAMPLE_FMT_S16,         ///< signed 16 bits
    AF_SAMPLE_FMT_S32,         ///< signed 32 bits
    AF_SAMPLE_FMT_FLT,         ///< float
    AF_SAMPLE_FMT_DBL,         ///< double

    AF_SAMPLE_FMT_U8P,         ///< unsigned 8 bits, planar
    AF_SAMPLE_FMT_S16P,        ///< signed 16 bits, planar
    AF_SAMPLE_FMT_S32P,        ///< signed 32 bits, planar
    AF_SAMPLE_FMT_FLTP,        ///< float, planar
    AF_SAMPLE_FMT_DBLP,        ///< double, planar

    AF_SAMPLE_FMT_NB           ///< Number of sample formats. DO NOT USE if linking dynamically
};
enum AFPixelFormat {
    AF_PIX_FMT_NONE = -1,
    AF_PIX_FMT_YUV420P,  ///< planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples)
    AF_PIX_FMT_YUYV422,  ///< packed YUV 4:2:2, 16bpp, Y0 Cb Y1 Cr
    AF_PIX_FMT_RGB24,    ///< packed RGB 8:8:8, 24bpp, RGBRGB...
    AF_PIX_FMT_BGR24,    ///< packed RGB 8:8:8, 24bpp, BGRBGR...
    AF_PIX_FMT_YUV422P,  ///< planar YUV 4:2:2, 16bpp, (1 Cr & Cb sample per 2x1 Y samples)
    AF_PIX_FMT_YUV444P,  ///< planar YUV 4:4:4, 24bpp, (1 Cr & Cb sample per 1x1 Y samples)
    AF_PIX_FMT_YUV410P,  ///< planar YUV 4:1:0,  9bpp, (1 Cr & Cb sample per 4x4 Y samples)
    AF_PIX_FMT_YUV411P,  ///< planar YUV 4:1:1, 12bpp, (1 Cr & Cb sample per 4x1 Y samples)
    AF_PIX_FMT_GRAY8,    ///<        Y        ,  8bpp
    AF_PIX_FMT_MONOWHITE,///<        Y        ,  1bpp, 0 is white, 1 is black, in each byte pixels are ordered from the msb to the lsb
    AF_PIX_FMT_MONOBLACK,///<        Y        ,  1bpp, 0 is black, 1 is white, in each byte pixels are ordered from the msb to the lsb
    AF_PIX_FMT_PAL8,     ///< 8 bits with AV_PIX_FMT_RGB32 palette
    AF_PIX_FMT_YUVJ420P, ///< planar YUV 4:2:0, 12bpp, full scale (JPEG), deprecated in favor of AV_PIX_FMT_YUV420P and setting color_range
    AF_PIX_FMT_YUVJ422P, ///< planar YUV 4:2:2, 16bpp, full scale (JPEG), deprecated in favor of AV_PIX_FMT_YUV422P and setting color_range
    AF_PIX_FMT_YUVJ444P, ///< planar YUV 4:4:4, 24bpp, full scale (JPEG), deprecated in favor of AV_PIX_FMT_YUV444P and setting color_range
    AF_PIX_FMT_UYVY422,  ///< packed YUV 4:2:2, 16bpp, Cb Y0 Cr Y1
    AF_PIX_FMT_UYYVYY411,///< packed YUV 4:1:1, 12bpp, Cb Y0 Y1 Cr Y2 Y3
    AF_PIX_FMT_BGR8,     ///< packed RGB 3:3:2,  8bpp, (msb)2B 3G 3R(lsb)
    AF_PIX_FMT_BGR4,///< packed RGB 1:2:1 bitstream,  4bpp, (msb)1B 2G 1R(lsb), a byte contains two pixels, the first pixel in the byte is the one composed by the 4 msb bits
    AF_PIX_FMT_BGR4_BYTE,///< packed RGB 1:2:1,  8bpp, (msb)1B 2G 1R(lsb)
    AF_PIX_FMT_RGB8,     ///< packed RGB 3:3:2,  8bpp, (msb)2R 3G 3B(lsb)
    AF_PIX_FMT_RGB4,///< packed RGB 1:2:1 bitstream,  4bpp, (msb)1R 2G 1B(lsb), a byte contains two pixels, the first pixel in the byte is the one composed by the 4 msb bits
    AF_PIX_FMT_RGB4_BYTE,///< packed RGB 1:2:1,  8bpp, (msb)1R 2G 1B(lsb)
    AF_PIX_FMT_NV12,///< planar YUV 4:2:0, 12bpp, 1 plane for Y and 1 plane for the UV components, which are interleaved (first byte U and the following byte V)
    AF_PIX_FMT_NV21,///< as above, but U and V bytes are swapped


    AF_PIX_FMT_YUV420P10BE = 63,///< planar YUV 4:2:0, 15bpp, (1 Cr & Cb sample per 2x2 Y samples), big-endian
    AF_PIX_FMT_YUV420P10LE,     ///< planar YUV 4:2:0, 15bpp, (1 Cr & Cb sample per 2x2 Y samples), little-endian

    /*
     * 硬件解码器的“原生句柄”格式（不是 CPU 可读的像素数据，data[] 里放的是
     * GPU 对象/ID，绝对不能交给 swscale 或 QImage）。
     * 值从 900 起，刻意避开上面那些与 AV_PIX_FMT_* 数值对齐的普通格式。
     */
    AF_PIX_FMT_D3D11 = 900,       ///< data[0] = ID3D11Texture2D*，data[1] = 数组切片号
    AF_PIX_FMT_DXVA2_VLD,
    AF_PIX_FMT_VAAPI = 902,       ///< Linux：data[3] = VASurfaceID（VAAPI 表面）

    AF_PIX_FMT_APPLE_PIXEL_BUFFER = 1000,
    AF_PIX_FMT_CICADA_AF,         //framework VideoFrame
    AF_PIX_FMT_CICADA_MEDIA_CODEC,//Android mediacodec buffer index
    AF_PIX_FMT_CICADA_TEXTURE,    //texture frame
};

/**
  * Chromaticity coordinates of the source primaries.
  * These values match the ones defined by ISO/IEC 23001-8_2013 § 7.1.
  */
enum AFColorPrimaries {
    AFCOL_PRI_RESERVED0 = 0,
    AFCOL_PRI_BT709 = 1,///< also ITU-R BT1361 / IEC 61966-2-4 / SMPTE RP177 Annex B
    AFCOL_PRI_UNSPECIFIED = 2,
    AFCOL_PRI_RESERVED = 3,
    AFCOL_PRI_BT470M = 4,///< also FCC Title 47 Code of Federal Regulations 73.682 (a)(20)

    AFCOL_PRI_BT470BG = 5,  ///< also ITU-R BT601-6 625 / ITU-R BT1358 625 / ITU-R BT1700 625 PAL & SECAM
    AFCOL_PRI_SMPTE170M = 6,///< also ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC
    AFCOL_PRI_SMPTE240M = 7,///< functionally identical to above
    AFCOL_PRI_FILM = 8,     ///< colour filters using Illuminant C
    AFCOL_PRI_BT2020 = 9,   ///< ITU-R BT2020
    AFCOL_PRI_SMPTE428 = 10,///< SMPTE ST 428-1 (CIE 1931 XYZ)
    AFCOL_PRI_SMPTEST428_1 = AFCOL_PRI_SMPTE428,
    AFCOL_PRI_SMPTE431 = 11, ///< SMPTE ST 431-2 (2011) / DCI P3
    AFCOL_PRI_SMPTE432 = 12, ///< SMPTE ST 432-1 (2010) / P3 D65 / Display P3
    AFCOL_PRI_JEDEC_P22 = 22,///< JEDEC P22 phosphors
    AFCOL_PRI_NB             ///< Not part of ABI
};

/**
 * Color Transfer Characteristic.
 * These values match the ones defined by ISO/IEC 23001-8_2013 § 7.2.
 */
enum AFColorTransferCharacteristic {
    AFCOL_TRC_RESERVED0 = 0,
    AFCOL_TRC_BT709 = 1,///< also ITU-R BT1361
    AFCOL_TRC_UNSPECIFIED = 2,
    AFCOL_TRC_RESERVED = 3,
    AFCOL_TRC_GAMMA22 = 4,  ///< also ITU-R BT470M / ITU-R BT1700 625 PAL & SECAM
    AFCOL_TRC_GAMMA28 = 5,  ///< also ITU-R BT470BG
    AFCOL_TRC_SMPTE170M = 6,///< also ITU-R BT601-6 525 or 625 / ITU-R BT1358 525 or 625 / ITU-R BT1700 NTSC
    AFCOL_TRC_SMPTE240M = 7,
    AFCOL_TRC_LINEAR = 8,       ///< "Linear transfer characteristics"
    AFCOL_TRC_LOG = 9,          ///< "Logarithmic transfer characteristic (100:1 range)"
    AFCOL_TRC_LOG_SQRT = 10,    ///< "Logarithmic transfer characteristic (100 * Sqrt(10) : 1 range)"
    AFCOL_TRC_IEC61966_2_4 = 11,///< IEC 61966-2-4
    AFCOL_TRC_BT1361_ECG = 12,  ///< ITU-R BT1361 Extended Colour Gamut
    AFCOL_TRC_IEC61966_2_1 = 13,///< IEC 61966-2-1 (sRGB or sYCC)
    AFCOL_TRC_BT2020_10 = 14,   ///< ITU-R BT2020 for 10-bit system
    AFCOL_TRC_BT2020_12 = 15,   ///< ITU-R BT2020 for 12-bit system
    AFCOL_TRC_SMPTE2084 = 16,   ///< SMPTE ST 2084 for 10-, 12-, 14- and 16-bit systems
    AFCOL_TRC_SMPTEST2084 = AFCOL_TRC_SMPTE2084,
    AFCOL_TRC_SMPTE428 = 17,///< SMPTE ST 428-1
    AFCOL_TRC_SMPTEST428_1 = AFCOL_TRC_SMPTE428,
    AFCOL_TRC_ARIB_STD_B67 = 18,///< ARIB STD-B67, known as "Hybrid log-gamma"
    AFCOL_TRC_NB                ///< Not part of ABI
};

/**
 * YUV colorspace type.
 * These values match the ones defined by ISO/IEC 23001-8_2013 § 7.3.
 */
enum AFColorSpace {
    AFCOL_SPC_RGB = 0,  ///< order of coefficients is actually GBR, also IEC 61966-2-1 (sRGB)
    AFCOL_SPC_BT709 = 1,///< also ITU-R BT1361 / IEC 61966-2-4 xvYCC709 / SMPTE RP177 Annex B
    AFCOL_SPC_UNSPECIFIED = 2,
    AFCOL_SPC_RESERVED = 3,
    AFCOL_SPC_FCC = 4,      ///< FCC Title 47 Code of Federal Regulations 73.682 (a)(20)
    AFCOL_SPC_BT470BG = 5,  ///< also ITU-R BT601-6 625 / ITU-R BT1358 625 / ITU-R BT1700 625 PAL & SECAM / IEC 61966-2-4 xvYCC601
    AFCOL_SPC_SMPTE170M = 6,///< also ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC
    AFCOL_SPC_SMPTE240M = 7,///< functionally identical to above
    AFCOL_SPC_YCGCO = 8,    ///< Used by Dirac / VC-2 and H.264 FRext, see ITU-T SG16
    AFCOL_SPC_YCOCG = AFCOL_SPC_YCGCO,
    AFCOL_SPC_BT2020_NCL = 9,         ///< ITU-R BT2020 non-constant luminance system
    AFCOL_SPC_BT2020_CL = 10,         ///< ITU-R BT2020 constant luminance system
    AFCOL_SPC_SMPTE2085 = 11,         ///< SMPTE 2085, Y'D'zD'x
    AFCOL_SPC_CHROMA_DERIVED_NCL = 12,///< Chromaticity-derived non-constant luminance system
    AFCOL_SPC_CHROMA_DERIVED_CL = 13, ///< Chromaticity-derived constant luminance system
    AFCOL_SPC_ICTCP = 14,             ///< ITU-R BT.2100-0, ICtCp
    AFCOL_SPC_NB                      ///< Not part of ABI
};

/**
 * MPEG vs JPEG YUV range.
 */
enum AFColorRange {
    AFCOL_RANGE_UNSPECIFIED = 0,
    AFCOL_RANGE_MPEG = 1,///< the normal 219*2^(n-8) "MPEG" YUV ranges
    AFCOL_RANGE_JPEG = 2,///< the normal     2^n-1   "JPEG" YUV ranges
    AFCOL_RANGE_NB       ///< Not part of ABI
};

/**
 * Location of chroma samples.
 *
 * Illustration showing the location of the first (top left) chroma sample of the
 * image, the left shows only luma, the right
 * shows the location of the chroma sample, the 2 could be imagined to overlay
 * each other but are drawn separately due to limitations of ASCII
 *
 *                1st 2nd       1st 2nd horizontal luma sample positions
 *                 v   v         v   v
 *                 ______        ______
 *1st luma line > |X   X ...    |3 4 X ...     X are luma samples,
 *                |             |1 2           1-6 are possible chroma positions
 *2nd luma line > |X   X ...    |5 6 X ...     0 is undefined/unknown position
 */
enum AFChromaLocation {
    AFCHROMA_LOC_UNSPECIFIED = 0,
    AFCHROMA_LOC_LEFT = 1,   ///< MPEG-2/4 4:2:0, H.264 default for 4:2:0
    AFCHROMA_LOC_CENTER = 2, ///< MPEG-1 4:2:0, JPEG 4:2:0, H.263 4:2:0
    AFCHROMA_LOC_TOPLEFT = 3,///< ITU-R 601, SMPTE 274M 296M S314M(DV 4:1:1), mpeg2 4:2:2
    AFCHROMA_LOC_TOP = 4,
    AFCHROMA_LOC_BOTTOMLEFT = 5,
    AFCHROMA_LOC_BOTTOM = 6,
    AFCHROMA_LOC_NB///< Not part of ABI
};

typedef enum InterlacedType_t {
    InterlacedType_UNKNOWN = -1,
    InterlacedType_NO,
    InterlacedType_YES
} InterlacedType;

typedef struct {
    int nChannles;
    int sample_rate;
    enum AFCodecID codec;
    enum AFSampleFormat sample_fmt;
    uint64_t channel_layout;
    int frame_size;
    uint8_t *extradata;
    int extradata_size;
    int bits_per_coded_sample;
    int nb_frame;

} audio_info;

typedef enum {
    picture_cache_type_unknown = -1,
    picture_cache_type_soft,
    picture_cache_type_cannot

} picture_cache_type;

typedef struct {
    int pix_fmt;
    int width;
    int height;
    picture_cache_type cache_type;

} video_info;

/**
 * The stream is stored in the file as an attached picture/"cover art" (e.g.
 * APIC frame in ID3v2). The single packet associated with it will be returned
 * among the first few packets read from the file unless seeking takes place.
 * It can also be accessed at any time in AVStream.attached_pic.
 */
#ifndef AV_DISPOSITION_ATTACHED_PIC
/*
 * 与 ffmpeg 的 avformat.h 保持**逐字相同**的写法（ffmpeg 用 `(1 << 10)`）：值本来就一样，
 * 但 MSVC 的 C4005 比较的是记号序列而不是取值，`0x0400` 与 `(1 << 10)` 会被判成
 * "宏重定义"并报一条警告。本工程要求零警告，故这里照 ffmpeg 的写法写。
 */
#define AV_DISPOSITION_ATTACHED_PIC      (1 << 10)
#endif

typedef struct Source_meta {
    char *key;
    char *value;
    struct Source_meta *next;
} Source_meta;


typedef struct VideoColorInfo {
    enum AFColorRange color_range;
    enum AFColorPrimaries color_primaries;
    enum AFColorTransferCharacteristic color_trc;
    enum AFColorSpace color_space;
    enum AFChromaLocation chroma_location;
} VideoColorInfo;

typedef struct {
    int64_t totalBitrate;
} Media_meta;

typedef struct {
    Stream_type type;
    int64_t duration;
    enum AFCodecID codec;
    uint32_t codec_tag;
    int index;
    int nb_index_entries;
    void *cicada_codec_context;
    int cicada_codec_context_size;

    char *title;
    char *language;
    int64_t seeked_time;

    int disposition; /**< AV_DISPOSITION_* bit field */

    int64_t bitrate;

    // TODO:  use union
    //audio
    int channels;
    uint64_t channel_layout;
    int samplerate;
    int frame_size;
    int profile;
    int bits_per_coded_sample;
    enum AFSampleFormat sample_fmt;

    //video only
    int width;
    int height;
    int rotate;
    int displayWidth;
    int displayHeight;
    double avg_fps;
    enum AFPixelFormat pixel_fmt;
    /**
 * Video only. Additional colorspace characteristics.
 */
    VideoColorInfo color_info;

    int pid;
    int no_program;
    int attached_pic;
    uint8_t *extradata;
    int extradata_size;
    InterlacedType interlaced;

    char *lang;
    uint64_t bandwidth;

    char *description;

    Source_meta *meta;

    //add for stand drm(WideVine,FairPlay...)
    char* keyUrl;
    char* keyFormat;
    // DRM init data (base64 PSSH) and default key id from ContentProtection,
    // populated for object-based (manifest) playback.
    char* drmPssh;
    char* drmKeyId;

    float ptsTimeBase;

    // add for dash
    int64_t suggestedPresentationDelay;

} Stream_meta;

/*
 * ---- 编码短名归一化（L1 公共实现，只有这一份） ----
 *
 * 应用层（StreamInfo.videoCodec，见 mediaPlayer/native_cicada_player_def.h）和
 * ABR（mediaPlayer/abr/AbrAlgoStrategy）都用下面的函数，不允许各自再解析一遍。
 *
 * afCodecShortName() 的两种输入形态，按此顺序取值：
 *   1) id != AF_CODEC_ID_NONE：容器/解码器/清单映射出来的真值，最可信；
 *   2) 否则看 rawCodecs —— 清单里的原始 codecs 字符串（DASH 的 @codecs、
 *      HLS 的 CODECS、MediaManifest 的 "codecs" 字段），是逗号分隔的
 *      RFC 6381 token 列表，大小写不敏感，token 两端可以有引号/空白；
 *   3) 两者都对不上：返回空串（"没有编码信息"），绝不猜。
 *
 * 返回值是**静态字符串常量**，调用方不得释放，也不需要拷贝。
 * 输出的短名只有这几个：H.264 / H.265 / AV1 / VP9 / MPEG-4 / MPEG-2；
 * 其余（含 VP8、Dolby Vision、以及各种音频编码）一律返回空串。
 */
const char *afCodecShortName(enum AFCodecID id, const char *rawCodecs);

/*
 * 清单原始 codecs 字符串 -> AFCodecID。
 * 只认视频编码：一个 token 列表里混着 "avc1.64001f,mp4a.40.2" 时，
 * 要的是视频那个；识别不出任何视频编码时返回 AF_CODEC_ID_NONE。
 */
enum AFCodecID afCodecIDFromManifestCodecs(const char *rawCodecs);

/*
 * 编码的压缩率/效率等级，数值越大越省带宽：
 *   AV1 = 5，H.265 = 4，VP9 = 3，H.264 = 2，MPEG-4 / MPEG-2 = 1，
 *   空串/未知 = 0。
 * ABR 的"同一分辨率内优先更省带宽的编码"用这个等级判断；等级表只有这一份。
 */
int afCodecEfficiencyRankByShortName(const char *shortName);

/* afCodecShortName() 的便捷形式：先归一化再取等级。 */
int afCodecEfficiencyRank(enum AFCodecID id, const char *rawCodecs);

/*
 * 内核默认的编码效率序，**从高到低**：AV1、H.265、VP9、H.264、MPEG-4、MPEG-2。
 *
 * 返回以 nullptr 结尾的静态数组，元素是 afCodecShortName() 的那组静态短名
 * （调用方不得释放、不得改写）。数组内容由 afCodecEfficiencyRankByShortName()
 * 的等级表在首次调用时**排出来**（同级保持名字表内次序），所以内核里"效率序"
 * 只有等级表这一个定义点，这里不会与它漂移；调用方也不允许再抄一份名字列表。
 *
 * 用途：应用层没传 preference 时，ABR 与起播默认档用它当默认效率序；
 * CicadaGetVideoCodecSupport 也用同一份把它交给应用层。
 */
const char *const *afCodecEfficiencyOrder();

/*
 * 短名 -> AFCodecID，afCodecShortName() 的逆映射。存在的唯一理由是：应用层拿到的
 * 是 StreamInfo.videoCodec 这个短名字符串（见 mediaPlayer/native_cicada_player_def.h），
 * 而"设备能不能硬解这个编码"是按 AFCodecID 问的（decoderFactory::isHardwareDecodeSupported），
 * 两边必须用**同一张**编码表，不允许调用方自己写 if-else 串。
 *
 * 认不出来（含 nullptr / 空串 / 未知短名）返回 AF_CODEC_ID_NONE。
 *
 * 注意 MPEG-2：AFCodecID 里没有对应的枚举项（见 afCodecShortNameFromManifestCodecs()
 * 里的说明），所以短名 "MPEG-2" 也会得到 AF_CODEC_ID_NONE。调用方**不得**据此
 * 推断"这个编码不存在"—— AF_CODEC_ID_NONE 的语义是"编码未知"，能力查询会按
 * "未知 = 视为都支持"处理。
 */
enum AFCodecID afCodecIDFromShortName(const char *shortName);


enum color_space {
    COLOR_SPACE_UNSPECIFIED = 0,
    COLOR_SPACE_BT709 = 1,
    COLOR_SPACE_BT601 = 2,
    COLOR_SPACE_BT2020 = 6
};

enum color_range {
    COLOR_RANGE_UNSPECIFIED = 0,
    COLOR_RANGE_FULL = 1,
    COLOR_RANGE_LIMITIED = 2,
};

#define MAX_AUDIO_FRAME_SIZE 192000

typedef int  (*decoder_buf_callback)(unsigned char *buffer[], int nb_samples, int line_size,
                                     long long pts,
                                     void *CbpHandle);

typedef int  (*decoder_buf_callback_video)(unsigned char *buffer[], int linesize[], long long pts,
                                           int type,
                                           void *CbpHandle);

enum dec_flag {
    dec_flag_dummy,
    dec_flag_hw,
    dec_flag_sw,
    dec_flag_out,
    dec_flag_direct,
    dec_flag_adaptive,
    dec_flag_passthrough_info,
    // adjust setting to output frames as soon as possiable.
    dec_flag_output_frame_asap,
    /*
     * B2：pending（切档目标）解码器用 1x1 占位 Surface 配置（surface 模式），
     * 提交时再 setOutputSurface 换到真 Surface。**只能追加在末尾**：
     * 改动既有枚举项的位序/取值会让所有已按旧位序编译的目标文件错位。
     * 位值 = bit8（0x100），与既有 bit0~bit7（dummy/hw/sw/out/direct/adaptive/
     * passthrough_info/output_frame_asap）都不冲突。
     */
    dec_flag_placeholder_surface,
};
#define DECFLAG_DUMMY  1u << dec_flag_dummy
#define DECFLAG_HW     (1u << dec_flag_hw)
#define DECFLAG_SW     (1u << dec_flag_sw)
#define DECFLAG_OUT    (1u << dec_flag_out)
#define DECFLAG_DIRECT (1u << dec_flag_direct)
#define DECFLAG_ADAPTIVE (1u << dec_flag_adaptive)
#define DECFLAG_PASSTHROUGH_INFO (1 << dec_flag_passthrough_info)
#define DECFLAG_OUTPUT_FRAME_ASAP (1u << dec_flag_output_frame_asap)
#define DECFLAG_PLACEHOLDER_SURFACE (1u << dec_flag_placeholder_surface)

typedef struct mediaFrame_t mediaFrame;

typedef void (*FrameRelease)(void *arg, mediaFrame *pFrame);

typedef enum {
    SKIP_OUTPUT_FALSE = 0,
    SKIP_OUTPUT_TRUE = 1,
} SKIP_OUTPUT;

typedef struct mediaFrame_t {
    SKIP_OUTPUT skipOutput;
    uint8_t *pBuffer;
    int size;
    int streamIndex;
    int64_t pts;
    int64_t dts;
    int flag;
    int duration;
    int64_t pos;

    int64_t startTime;
    FrameRelease release;
    void *release_arg;

    void *pAppending;

    int width;
    int height;
} mediaFrame;

enum callback_cmd {
    callback_cmd_get_source_meta,
    callback_cmd_get_sink_meta,
    callback_cmd_get_frame
};

#define MKTAG(a, b, c, d) ((a) | ((b) << 8) | ((c) << 16) | ((unsigned)(d) << 24))
#define CICADAERRTAG(a, b, c, d) (-(int)MKTAG(a, b, c, d))
#define SEGEND   CICADAERRTAG(0xF9,'S','E','D')

#ifdef WIN32

#if (_MSC_VER >= 1700) && !defined(_USING_V110_SDK71_)
#include <d3d11.h>
typedef struct CicadaD3D11VADeviceContext {
    /**
     * Device used for texture creation and access. This can also be used to
     * set the libavcodec decoding device.
     *
     * Must be set by the user. This is the only mandatory field - the other
     * device context fields are set from this and are available for convenience.
     *
     * Deallocating the AVHWDeviceContext will always release this interface,
     * and it does not matter whether it was user-allocated.
     */
    ID3D11Device        *device;

    /**
     * If unset, this will be set from the device field on init.
     *
     * Deallocating the AVHWDeviceContext will always release this interface,
     * and it does not matter whether it was user-allocated.
     */
    ID3D11DeviceContext *device_context;

    /**
     * If unset, this will be set from the device field on init.
     *
     * Deallocating the AVHWDeviceContext will always release this interface,
     * and it does not matter whether it was user-allocated.
     */
    ID3D11VideoDevice   *video_device;

    /**
     * If unset, this will be set from the device_context field on init.
     *
     * Deallocating the AVHWDeviceContext will always release this interface,
     * and it does not matter whether it was user-allocated.
     */
    ID3D11VideoContext  *video_context;

    /**
     * Callbacks for locking. They protect accesses to device_context and
     * video_context calls. They also protect access to the internal staging
     * texture (for av_hwframe_transfer_data() calls). They do NOT protect
     * access to hwcontext or decoder state in general.
     *
     * If unset on init, the hwcontext implementation will set them to use an
     * internal mutex.
     *
     * The underlying lock must be recursive. lock_ctx is for free use by the
     * locking implementation.
     */
    void (*lock)(void *lock_ctx);
    void (*unlock)(void *lock_ctx);
    void *lock_ctx;
} CicadaD3D11VADeviceContext;
#endif
#endif

typedef enum AFHWDeviceType {
    AF_HWDEVICE_TYPE_UNKNOWN = -1,
    AF_HWDEVICE_TYPE_D3D11VA = 0,
} AFHWDeviceType;


#endif
