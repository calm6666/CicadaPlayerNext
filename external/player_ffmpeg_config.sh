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
