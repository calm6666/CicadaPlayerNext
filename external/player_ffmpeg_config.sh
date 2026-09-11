#!/usr/bin/env bash
# ============================================================================
# FFmpeg 9.0 component configuration for CicadaPlayerNext
#
# The component list is verified against `ffmpeg configure --list-*` output
# of FFmpeg 9.0 (ffmpeg_commands.sh exits the build when a component name is
# not supported, so every name below must exist in FFmpeg 9.0).
#
# libavresample is gone since FFmpeg 5.0 — resampling uses libswresample.
# ============================================================================

function dav1d_decoder_prebuilt() {
    if [[ -n "${DAV1D_EXTERNAL_DIR}" ]];then
        if [[ -d "${DAV1D_EXTERNAL_DIR}/$TARGET_PLATFORM/$TARGET_ARCH" ]];then
            DAV1D_INSTALL_DIR="${DAV1D_EXTERNAL_DIR}/$TARGET_PLATFORM/$TARGET_ARCH"
        else
            DAV1D_INSTALL_DIR=
        fi
    fi

    echo "DAV1D_INSTALL_DIR is $DAV1D_INSTALL_DIR"
}

# ---- decoders ---------------------------------------------------------------
ffmpeg_config_add_decoders \
    aac aac_latm aac_fixed \
    h264 hevc mpeg4 mpeg2video mpeg1video mjpeg \
    mp3 mp3adu mp3float mp3on4float mp3adufloat mp3on4 \
    pcm_s16le pcm_s16be pcm_s24le pcm_f32le \
    ac3 eac3 dca opus vorbis flac alac \
    vp8 vp9 av1 \
    ass srt subrip webvtt text movtext

# AudioToolbox 解码器仅 Apple 平台可用（依赖 audiotoolbox 框架）
if [[ "$TARGET_PLATFORM" == "iOS" || "$TARGET_PLATFORM" == "Darwin" || "$TARGET_PLATFORM" == "maccatalyst" ]]; then
    ffmpeg_config_add_decoders ac3_at eac3_at
fi

# ---- demuxers ---------------------------------------------------------------
ffmpeg_config_add_demuxers \
    flv live_flv aac h264 hevc \
    mov mp4 m4v mp3 mpegts mpegps matroska av1 \
    webvtt srt ass ac3 eac3 ogg wav \
    hls dash concat

# ---- muxers -----------------------------------------------------------------
# 注：FFmpeg 没有 "aac" muxer（AAC 封装用 adts）
ffmpeg_config_add_muxers mp4 adts mpegts flv h264 hevc

# ---- parsers ----------------------------------------------------------------
ffmpeg_config_add_parsers \
    aac aac_latm h264 hevc mpeg4video mpegaudio \
    vp9 av1 opus ac3 flac vorbis mpegvideo

# ---- bitstream filters ------------------------------------------------------
ffmpeg_config_add_bsfs \
    aac_adtstoasc h264_mp4toannexb hevc_mp4toannexb \
    extract_extradata vp9_superframe_split av1_frame_merge av1_metadata \
    mpeg4_unpack_bframes

# ---- protocols --------------------------------------------------------------
# 注：hls "protocol" 已在 FFmpeg 5.0 移除（播放走 hls demuxer，已在上方启用）
ffmpeg_config_add_protocols \
    file rtmp http https crypto data tcp pipe subfile concat \
    async cache icecast httpproxy

# ---- filters ----------------------------------------------------------------
ffmpeg_config_add_filters \
    atempo aresample aformat volume scale transpose hflip vflip rotate

if [[ "$TARGET_PLATFORM" != "Android" ]];then
    ffmpeg_config_add_protocols udp
fi

# ---- hardware accelerators (Windows only) ------------------------------------
# ffmpeg_disable_all_config (build_tools/ffmpeg_commands.sh) starts from
# --disable-everything --disable-hwaccels --disable-dxva2, and nothing here ever
# re-enabled a hwaccel -- so external/install/ffmpeg/win32/*/libffmpeg.dll ships
# with no hardware decoder at all (only the d3d11va *device* module, which the
# --disable-hwaccels option does not touch). framework/codec/avcodecDecoder.cpp
# therefore has nothing to drive, and Windows always fell back to software.
#
# D3D11VA is the only usable modern backend here: DXVA2 is disabled outright
# (build_tools/ffmpeg_cross_compile_config.sh), and hwcontext_d3d11va is already
# built. No extra system libraries are needed for the hwaccel itself:
# libavutil/hwcontext_d3d11va.c loads d3d11.dll/dxgi.dll at runtime through
# dlopen + GetProcAddress, which is why link_shared_lib_win32() can keep linking
# with only -lws2_32 -lbcrypt -lcrypt32 under -Wl,--no-undefined.
#
# The *_d3d11va2 names matter: FFmpeg defines two D3D11VA hwaccels per codec,
# and only the "2" one is the modern API:
#   h264_d3d11va   -> .p.pix_fmt = AV_PIX_FMT_D3D11VA_VLD  (legacy; frame->data[3]
#                     holds a bare ID3D11VideoDecoderOutputView and there is no
#                     AVHWFramesContext, so av_hwframe_transfer_data() cannot work)
#   h264_d3d11va2  -> .p.pix_fmt = AV_PIX_FMT_D3D11        (FFmpeg owns a frames
#                     context, which is what the copy-back in
#                     framework/codec/avcodecDecoder.cpp needs)
#
# BOTH variants of a codec must be enabled together -- this is not optional:
# libavcodec/Makefile only wires the object file for the non-"2" hwaccel
#   OBJS-$(CONFIG_H264_D3D11VA_HWACCEL)  += dxva2_h264.o
#   OBJS-$(CONFIG_H264_DXVA2_HWACCEL)    += dxva2_h264.o
#   (there is no OBJS-$(CONFIG_H264_D3D11VA2_HWACCEL) rule at all)
# while both structs live in that single object, guarded separately:
#   dxva2_h264.c: #if CONFIG_H264_D3D11VA_HWACCEL  -> ff_h264_d3d11va_hwaccel
#   dxva2_h264.c: #if CONFIG_H264_D3D11VA2_HWACCEL -> ff_h264_d3d11va2_hwaccel
# Enabling only the "2" name therefore sets the macro that makes h264dec.c
# reference ff_h264_d3d11va2_hwaccel without ever compiling the object that
# defines it, and the merged DLL link dies with
#   undefined reference to `ff_h264_d3d11va2_hwaccel'
# (same for hevc/mpeg2/vp9/av1). Enabling both compiles the object once and
# emits both structs; the decoder then reports both pixel formats through
# avcodec_get_hw_config(), and avcodecDecoder::hasD3D11vaHwConfig() explicitly
# selects the AV_PIX_FMT_D3D11 entry and ignores the legacy one.
#
# Every name must appear in `configure --list-hwaccels`; an unavailable one is
# reported as a non-fatal WARN by ffmpeg_verify_requested_components().
# Only codecs whose decoder is already enabled above are listed, so enabling a
# hwaccel cannot silently pull in an extra decoder.
#
# NOTE: TARGET_PLATFORM here is the value passed to build_external.sh, i.e.
# "Windows" -- NOT the internal "win32" token that build_win32.sh passes to
# build_libs()/ffmpeg_cross_compile_set_win32().
if [[ "$TARGET_PLATFORM" == "Windows" ]]; then
    ffmpeg_config_add_hwaccels \
        h264_d3d11va  h264_d3d11va2 \
        hevc_d3d11va  hevc_d3d11va2 \
        mpeg2_d3d11va mpeg2_d3d11va2 \
        vp9_d3d11va   vp9_d3d11va2 \
        av1_d3d11va   av1_d3d11va2
fi

# Optional: parse DASH MPD through FFmpeg's own demuxer (the player uses its
# own DASH implementation, so this stays commented).
# ffmpeg_config_add_demuxers dash

# https_protocol 依赖 tls/openssl。OpenSSL 3.0.x 由 build_openssl_3.sh 编译安装，
# 默认链入 FFmpeg，消除
# "WARNING: Disabled https_protocol because some selected dependency is unsatisfied: tls_protocol"。
# Apple 平台设 SSL_USE_NATIVE=TRUE 时仍走 securetransport（build_ffmpeg.sh 判断）。
if [[ "${FFMPEG_USE_OPENSSL}" != "FALSE" ]];then
  FFMPEG_USE_OPENSSL="TRUE"
fi
