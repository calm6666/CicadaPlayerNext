//
// sampleDecryptDec.c (REMOVED FROM BUILD — kept for history)
//
// This file contained a custom AVInputFormat demuxer
// ("sampleDecryp" demuxer with read_header/read_packet callbacks that
// decrypted sample-level-encrypted packets in-place).
//
// FFmpeg 6.1+ moved the demuxer callbacks from the public AVInputFormat into
// the internal FFInputFormat and removed av_register_input_format(), so this
// file can no longer compile against FFmpeg 9.0. The functionality moved to
// SampleDecryptDemuxer.cpp, which now demuxes with the standard
// avFormatDemuxer and decrypts packets in ReadPacket().
//
// See docs/FFmpeg9_Upgrade.md for the full migration inventory.
//
