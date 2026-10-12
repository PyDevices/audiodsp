// Every external symbol Tremor defines, renamed into audiodsp's namespace.
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiodsp_vorbis.c includes this before any of Tremor, so its declarations
// and its definitions both come out as audiodsp_tremor_*. Without it, a
// firmware that also links another copy of Tremor (a streaming client with
// its own) fails to link on the first duplicate ov_read.
//
// The list is every global symbol in the compiled translation unit that
// isn't audiodsp_vorbis_*. tests/parity/vorbis_symbols_check.py reads the
// object file and fails on any symbol that escaped, so a Tremor update that
// adds a function shows up there rather than at some firmware's link step.

#pragma once

#define FLOOR_fromdB_LOOKUP          audiodsp_tremor_FLOOR_fromdB_LOOKUP
#define _book_maptype1_quantvals     audiodsp_tremor__book_maptype1_quantvals
#define _ilog                        audiodsp_tremor__ilog
#define decode_map                   audiodsp_tremor_decode_map
#define floor0_free_info             audiodsp_tremor_floor0_free_info
#define floor0_info_unpack           audiodsp_tremor_floor0_info_unpack
#define floor0_inverse1              audiodsp_tremor_floor0_inverse1
#define floor0_inverse2              audiodsp_tremor_floor0_inverse2
#define floor0_memosize              audiodsp_tremor_floor0_memosize
#define floor1_free_info             audiodsp_tremor_floor1_free_info
#define floor1_info_unpack           audiodsp_tremor_floor1_info_unpack
#define floor1_inverse1              audiodsp_tremor_floor1_inverse1
#define floor1_inverse2              audiodsp_tremor_floor1_inverse2
#define floor1_memosize              audiodsp_tremor_floor1_memosize
#define mapping_clear_info           audiodsp_tremor_mapping_clear_info
#define mapping_info_unpack          audiodsp_tremor_mapping_info_unpack
#define mapping_inverse              audiodsp_tremor_mapping_inverse
#define mdct_backward                audiodsp_tremor_mdct_backward
#define mdct_shift_right             audiodsp_tremor_mdct_shift_right
#define mdct_unroll_lap              audiodsp_tremor_mdct_unroll_lap
#define ogg_buffer_dup               audiodsp_tremor_ogg_buffer_dup
#define ogg_packet_release           audiodsp_tremor_ogg_packet_release
#define ogg_page_bos                 audiodsp_tremor_ogg_page_bos
#define ogg_page_continued           audiodsp_tremor_ogg_page_continued
#define ogg_page_dup                 audiodsp_tremor_ogg_page_dup
#define ogg_page_eos                 audiodsp_tremor_ogg_page_eos
#define ogg_page_granulepos          audiodsp_tremor_ogg_page_granulepos
#define ogg_page_packets             audiodsp_tremor_ogg_page_packets
#define ogg_page_pageno              audiodsp_tremor_ogg_page_pageno
#define ogg_page_release             audiodsp_tremor_ogg_page_release
#define ogg_page_serialno            audiodsp_tremor_ogg_page_serialno
#define ogg_page_version             audiodsp_tremor_ogg_page_version
#define ogg_stream_create            audiodsp_tremor_ogg_stream_create
#define ogg_stream_destroy           audiodsp_tremor_ogg_stream_destroy
#define ogg_stream_packetout         audiodsp_tremor_ogg_stream_packetout
#define ogg_stream_packetpeek        audiodsp_tremor_ogg_stream_packetpeek
#define ogg_stream_pagein            audiodsp_tremor_ogg_stream_pagein
#define ogg_stream_reset             audiodsp_tremor_ogg_stream_reset
#define ogg_stream_reset_serialno    audiodsp_tremor_ogg_stream_reset_serialno
#define ogg_sync_bufferin            audiodsp_tremor_ogg_sync_bufferin
#define ogg_sync_create              audiodsp_tremor_ogg_sync_create
#define ogg_sync_destroy             audiodsp_tremor_ogg_sync_destroy
#define ogg_sync_pageout             audiodsp_tremor_ogg_sync_pageout
#define ogg_sync_pageseek            audiodsp_tremor_ogg_sync_pageseek
#define ogg_sync_reset               audiodsp_tremor_ogg_sync_reset
#define ogg_sync_wrote               audiodsp_tremor_ogg_sync_wrote
#define oggpack_adv                  audiodsp_tremor_oggpack_adv
#define oggpack_bits                 audiodsp_tremor_oggpack_bits
#define oggpack_bytes                audiodsp_tremor_oggpack_bytes
#define oggpack_eop                  audiodsp_tremor_oggpack_eop
#define oggpack_look                 audiodsp_tremor_oggpack_look
#define oggpack_read                 audiodsp_tremor_oggpack_read
#define oggpack_readinit             audiodsp_tremor_oggpack_readinit
#define ov_bitrate                   audiodsp_tremor_ov_bitrate
#define ov_bitrate_instant           audiodsp_tremor_ov_bitrate_instant
#define ov_clear                     audiodsp_tremor_ov_clear
#define ov_comment                   audiodsp_tremor_ov_comment
#define ov_info                      audiodsp_tremor_ov_info
#define ov_open                      audiodsp_tremor_ov_open
#define ov_open_callbacks            audiodsp_tremor_ov_open_callbacks
#define ov_pcm_seek                  audiodsp_tremor_ov_pcm_seek
#define ov_pcm_seek_page             audiodsp_tremor_ov_pcm_seek_page
#define ov_pcm_tell                  audiodsp_tremor_ov_pcm_tell
#define ov_pcm_total                 audiodsp_tremor_ov_pcm_total
#define ov_raw_seek                  audiodsp_tremor_ov_raw_seek
#define ov_raw_tell                  audiodsp_tremor_ov_raw_tell
#define ov_raw_total                 audiodsp_tremor_ov_raw_total
#define ov_read                      audiodsp_tremor_ov_read
#define ov_seekable                  audiodsp_tremor_ov_seekable
#define ov_serialnumber              audiodsp_tremor_ov_serialnumber
#define ov_streams                   audiodsp_tremor_ov_streams
#define ov_test                      audiodsp_tremor_ov_test
#define ov_test_callbacks            audiodsp_tremor_ov_test_callbacks
#define ov_test_open                 audiodsp_tremor_ov_test_open
#define ov_time_seek                 audiodsp_tremor_ov_time_seek
#define ov_time_seek_page            audiodsp_tremor_ov_time_seek_page
#define ov_time_tell                 audiodsp_tremor_ov_time_tell
#define ov_time_total                audiodsp_tremor_ov_time_total
#define res_clear_info               audiodsp_tremor_res_clear_info
#define res_inverse                  audiodsp_tremor_res_inverse
#define res_unpack                   audiodsp_tremor_res_unpack
#define vorbis_book_clear            audiodsp_tremor_vorbis_book_clear
#define vorbis_book_decode           audiodsp_tremor_vorbis_book_decode
#define vorbis_book_decodev_add      audiodsp_tremor_vorbis_book_decodev_add
#define vorbis_book_decodev_set      audiodsp_tremor_vorbis_book_decodev_set
#define vorbis_book_decodevs_add     audiodsp_tremor_vorbis_book_decodevs_add
#define vorbis_book_decodevv_add     audiodsp_tremor_vorbis_book_decodevv_add
#define vorbis_book_unpack           audiodsp_tremor_vorbis_book_unpack
#define vorbis_comment_clear         audiodsp_tremor_vorbis_comment_clear
#define vorbis_comment_init          audiodsp_tremor_vorbis_comment_init
#define vorbis_comment_query         audiodsp_tremor_vorbis_comment_query
#define vorbis_comment_query_count   audiodsp_tremor_vorbis_comment_query_count
#define vorbis_dsp_create            audiodsp_tremor_vorbis_dsp_create
#define vorbis_dsp_destroy           audiodsp_tremor_vorbis_dsp_destroy
#define vorbis_dsp_headerin          audiodsp_tremor_vorbis_dsp_headerin
#define vorbis_dsp_pcmout            audiodsp_tremor_vorbis_dsp_pcmout
#define vorbis_dsp_read              audiodsp_tremor_vorbis_dsp_read
#define vorbis_dsp_restart           audiodsp_tremor_vorbis_dsp_restart
#define vorbis_dsp_synthesis         audiodsp_tremor_vorbis_dsp_synthesis
#define vorbis_info_blocksize        audiodsp_tremor_vorbis_info_blocksize
#define vorbis_info_clear            audiodsp_tremor_vorbis_info_clear
#define vorbis_info_init             audiodsp_tremor_vorbis_info_init
#define vorbis_lsp_to_curve          audiodsp_tremor_vorbis_lsp_to_curve
#define vorbis_packet_blocksize      audiodsp_tremor_vorbis_packet_blocksize
