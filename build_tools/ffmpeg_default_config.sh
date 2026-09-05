#!/usr/bin/env bash

# Fallback FFmpeg component configuration (used only when the build directory
# has no *_ffmpeg_config.sh). The real configuration is
# external/player_ffmpeg_config.sh.

# NOTE: libfdk_aac requires --enable-libfdk-aac --enable-nonfree; the native
# AAC decoder does not. Use the native decoders here so the fallback always
# configures cleanly on FFmpeg 9.0.
ffmpeg_config_add_decoders aac h264

ffmpeg_config_add_encoders aac

#ffmpeg_config_add_demuxers flv

ffmpeg_config_add_muxers mp4

ffmpeg_config_add_parsers h264 aac
#ffmpeg_config_add_protocols http

ffmpeg_config_add_bsfs aac_adtstoasc h264_mp4toannexb

ffmpeg_config_add_filters atempo


#ffmpeg_config_add_user --enable-ffplay
#ffmpeg_config_add_user --enable-filter=aresample
