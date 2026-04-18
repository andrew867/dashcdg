#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "dashcdg/audio_jitter.h"
#include "dashcdg/cdg.h"
#include "dashcdg/cdg_batch_jitter.h"
#include "dashcdg/cdg_raster.h"
#include "dashcdg/common.h"
#include "dashcdg/fec.h"
#include "dashcdg/media_clock.h"
#include "dashcdg/nb_ima_codec.h"
#include "dashcdg/protocol.h"

static struct dashcdg_subchannel_packet make_packet(uint8_t instruction) {
    struct dashcdg_subchannel_packet pkt;

    memset(&pkt, 0, sizeof(pkt));
    pkt.command = 0x09;
    pkt.instruction = instruction;
    return pkt;
}

static void test_memory_and_border(void) {
    struct dashcdg_cdg_state state;
    struct dashcdg_subchannel_packet pkt = make_packet(DASHCDG_INSN_MEMORY_PRESET);
    struct dashcdg_insn_memory_preset *preset = (struct dashcdg_insn_memory_preset *) pkt.data;

    dashcdg_cdg_state_init(&state);
    preset->color = 3;
    preset->repeat = 0;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(10, 10)] == 3);

    pkt = make_packet(DASHCDG_INSN_BORDER_PRESET);
    ((struct dashcdg_insn_border_preset *) pkt.data)->color = 5;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(0, 0)] == 5);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(10, 20)] == 3);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(DASHCDG_SCREEN_WIDTH - 1, DASHCDG_SCREEN_HEIGHT - 1)] == 5);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(DASHCDG_VISIBLE_X, DASHCDG_VISIBLE_Y)] == 3);
}

static void test_tile_and_scroll_copy(void) {
    struct dashcdg_cdg_state state;
    struct dashcdg_subchannel_packet pkt = make_packet(DASHCDG_INSN_TILE_BLOCK);
    struct dashcdg_insn_tile_block *tile = (struct dashcdg_insn_tile_block *) pkt.data;

    dashcdg_cdg_state_init(&state);
    tile->color_0 = 1;
    tile->color_1 = 2;
    tile->row = 1;
    tile->column = 1;
    tile->pixels[0] = 0x20;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(6, 12)] == 2);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(7, 12)] == 1);

    pkt = make_packet(DASHCDG_INSN_SCROLL_COPY);
    ((struct dashcdg_insn_scroll *) pkt.data)->h_scroll = 0x10;
    ((struct dashcdg_insn_scroll *) pkt.data)->v_scroll = 0x00;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(12, 12)] == 2);
}

static void test_scroll_preset_and_transparency(void) {
    struct dashcdg_cdg_state state;
    struct dashcdg_subchannel_packet pkt = make_packet(DASHCDG_INSN_MEMORY_PRESET);

    dashcdg_cdg_state_init(&state);
    ((struct dashcdg_insn_memory_preset *) pkt.data)->color = 7;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);

    pkt = make_packet(DASHCDG_INSN_SCROLL_PRESET);
    ((struct dashcdg_insn_scroll *) pkt.data)->color = 4;
    ((struct dashcdg_insn_scroll *) pkt.data)->v_scroll = 0x10;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(10, 0)] == 4);
    assert(state.display_v_offset == 0);

    pkt = make_packet(DASHCDG_INSN_DEF_TRANSPARENT);
    for (int i = 0; i < DASHCDG_COLORS; ++i) {
        pkt.data[i] = (uint8_t) i;
    }
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.transparency[0] == 0);
    assert(state.transparency[15] == 15);
}

static void test_scroll_copy_direction_and_offset_clamp(void) {
    struct dashcdg_cdg_state state;
    struct dashcdg_subchannel_packet pkt = make_packet(DASHCDG_INSN_MEMORY_PRESET);

    dashcdg_cdg_state_init(&state);
    ((struct dashcdg_insn_memory_preset *) pkt.data)->color = 0;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);

    state.framebuffer[DASHCDG_ARRAY_INDEX(20, 30)] = 9;

    pkt = make_packet(DASHCDG_INSN_SCROLL_COPY);
    ((struct dashcdg_insn_scroll *) pkt.data)->h_scroll = 0x10;
    ((struct dashcdg_insn_scroll *) pkt.data)->v_scroll = 0x10;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.framebuffer[DASHCDG_ARRAY_INDEX(26, 42)] == 9);
    assert(state.display_h_offset == 0);
    assert(state.display_v_offset == 0);

    pkt = make_packet(DASHCDG_INSN_SCROLL_COPY);
    ((struct dashcdg_insn_scroll *) pkt.data)->h_scroll = 0x07;
    ((struct dashcdg_insn_scroll *) pkt.data)->v_scroll = 0x0f;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);
    assert(state.display_h_offset == 5);
    assert(state.display_v_offset == 11);
}

static void test_reader_seek_and_keyframes(void) {
    struct dashcdg_cdg_reader reader;
    struct dashcdg_subchannel_packet stream[4];

    dashcdg_cdg_reader_init(&reader);
    memset(stream, 0, sizeof(stream));

    stream[0] = make_packet(DASHCDG_INSN_MEMORY_PRESET);
    ((struct dashcdg_insn_memory_preset *) stream[0].data)->color = 1;

    stream[1] = make_packet(DASHCDG_INSN_TILE_BLOCK);
    ((struct dashcdg_insn_tile_block *) stream[1].data)->color_0 = 1;
    ((struct dashcdg_insn_tile_block *) stream[1].data)->color_1 = 9;
    ((struct dashcdg_insn_tile_block *) stream[1].data)->pixels[0] = 0x20;

    stream[2] = make_packet(DASHCDG_INSN_MEMORY_PRESET);
    ((struct dashcdg_insn_memory_preset *) stream[2].data)->color = 2;

    stream[3] = make_packet(DASHCDG_INSN_TILE_BLOCK);
    ((struct dashcdg_insn_tile_block *) stream[3].data)->color_0 = 2;
    ((struct dashcdg_insn_tile_block *) stream[3].data)->color_1 = 8;
    ((struct dashcdg_insn_tile_block *) stream[3].data)->pixels[0] = 0x20;

    assert(dashcdg_cdg_reader_load_memory(&reader, (const uint8_t *) stream, sizeof(stream)) == 1);
    assert(dashcdg_cdg_reader_build_keyframes(&reader) == 1);
    assert(reader.keyframes.count == 2);

    assert(dashcdg_cdg_reader_seek(&reader, 4) == 1);
    assert(reader.state.framebuffer[0] == 8);
    assert(dashcdg_cdg_reader_seek(&reader, 2) == 1);
    assert(reader.state.framebuffer[0] == 9);

    dashcdg_cdg_reader_free(&reader);
}

static void test_protocol_roundtrip(void) {
    uint8_t buffer[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header header;
    struct dashcdg_packet_view view;
    struct dashcdg_announce_payload announce;
    struct dashcdg_asset_chunk_payload chunk;
    struct dashcdg_clock_beacon_payload beacon;
    struct dashcdg_audio_frame_payload audio_frame;
    struct dashcdg_fec_parity_payload fec_parity;
    struct dashcdg_cdg_snapshot_payload cdg_snapshot;
    struct dashcdg_ptp_delay_req_payload delay_req;
    struct dashcdg_ptp_delay_resp_payload delay_resp;
    uint8_t chunk_bytes[] = { 1, 2, 3, 4 };
    uint8_t audio_bytes[] = { 9, 8, 7, 6 };
    uint8_t fec_bytes[] = { 0x10, 0x20, 0x30, 0x40 };
    uint8_t snapshot_bytes[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    size_t size;

    memset(&header, 0, sizeof(header));
    header.sequence = 42;
    header.sender_time_ms = 1234;

    memset(&announce, 0, sizeof(announce));
    strcpy(announce.song_id, "demo-song");
    announce.asset_size = 4096;
    announce.chunk_size = 1024;
    announce.packets_per_second = DASHCDG_PACKETS_PER_SECOND;
    announce.audio_sample_rate = 48000;
    announce.audio_channels = 2;
    announce.audio_frame_ms = 20;
    announce.audio_bitrate_kbps = 128;
    announce.playout_delay_ms = 500;
    announce.audio_fec_group_size = 5;
    announce.cdg_fec_group_size = 9;
    announce.session_start_ms = 5000;

    size = dashcdg_protocol_serialize_announce(buffer, sizeof(buffer), &header, &announce);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(strcmp(view.announce.song_id, "demo-song") == 0);
    assert(view.announce.asset_size == 4096);
    assert(view.announce.audio_sample_rate == 48000);
    assert(view.announce.audio_frame_ms == 20);

    chunk.asset_offset = 256;
    chunk.chunk_length = sizeof(chunk_bytes);
    chunk.reserved = 0;
    chunk.chunk_bytes = chunk_bytes;
    size = dashcdg_protocol_serialize_asset_chunk(buffer, sizeof(buffer), &header, &chunk);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.asset_chunk.asset_offset == 256);
    assert(view.asset_chunk.chunk_length == sizeof(chunk_bytes));
    assert(memcmp(view.asset_chunk.chunk_bytes, chunk_bytes, sizeof(chunk_bytes)) == 0);

    memset(&beacon, 0, sizeof(beacon));
    strcpy(beacon.song_id, "demo-song");
    beacon.session_start_ms = 5000;
    beacon.playback_ms = 123;
    beacon.available_asset_bytes = 4096;
    beacon.total_asset_bytes = 4096;
    size = dashcdg_protocol_serialize_clock_beacon(buffer, sizeof(buffer), &header, &beacon);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.clock_beacon.playback_ms == 123);

    memset(&audio_frame, 0, sizeof(audio_frame));
    audio_frame.media_sequence = 3;
    audio_frame.group_id = 1;
    audio_frame.group_index = 0;
    audio_frame.frame_ms = 20;
    audio_frame.encoded_length = sizeof(audio_bytes);
    audio_frame.playback_ms = 40;
    audio_frame.encoded_bytes = audio_bytes;
    size = dashcdg_protocol_serialize_audio_frame(buffer, sizeof(buffer), &header, &audio_frame);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.audio_frame.media_sequence == 3);
    assert(view.audio_frame.playback_ms == 40);
    assert(memcmp(view.audio_frame.encoded_bytes, audio_bytes, sizeof(audio_bytes)) == 0);

    memset(&delay_req, 0, sizeof(delay_req));
    delay_req.request_id = 77;
    size = dashcdg_protocol_serialize_ptp_delay_req(buffer, sizeof(buffer), &header, &delay_req);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.ptp_delay_req.request_id == 77);

    memset(&delay_resp, 0, sizeof(delay_resp));
    delay_resp.request_id = 77;
    delay_resp.request_rx_time_ms = 9876;
    size = dashcdg_protocol_serialize_ptp_delay_resp(buffer, sizeof(buffer), &header, &delay_resp);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.ptp_delay_resp.request_id == 77);
    assert(view.ptp_delay_resp.request_rx_time_ms == 9876);

    memset(&fec_parity, 0, sizeof(fec_parity));
    fec_parity.stream_type = DASHCDG_STREAM_TYPE_AUDIO;
    fec_parity.group_size = 5;
    fec_parity.payload_bytes = sizeof(fec_bytes);
    fec_parity.group_id = 1;
    fec_parity.payload_length_xor = sizeof(fec_bytes);
    fec_parity.payload_xor = fec_bytes;
    size = dashcdg_protocol_serialize_fec_parity(buffer, sizeof(buffer), &header, &fec_parity);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.fec_parity.stream_type == DASHCDG_STREAM_TYPE_AUDIO);
    assert(view.fec_parity.group_size == 5);
    assert(view.fec_parity.group_id == 1);
    assert(view.fec_parity.payload_length_xor == sizeof(fec_bytes));
    assert(memcmp(view.fec_parity.payload_xor, fec_bytes, sizeof(fec_bytes)) == 0);

    memset(&cdg_snapshot, 0, sizeof(cdg_snapshot));
    cdg_snapshot.snapshot_id = 5;
    cdg_snapshot.packet_index = 1234;
    cdg_snapshot.total_bytes = 32;
    cdg_snapshot.snapshot_offset = 7;
    cdg_snapshot.chunk_length = sizeof(snapshot_bytes);
    cdg_snapshot.snapshot_bytes = snapshot_bytes;
    size = dashcdg_protocol_serialize_cdg_snapshot(buffer, sizeof(buffer), &header, &cdg_snapshot);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.cdg_snapshot.snapshot_id == 5);
    assert(view.cdg_snapshot.packet_index == 1234);
    assert(view.cdg_snapshot.total_bytes == 32);
    assert(view.cdg_snapshot.snapshot_offset == 7);
    assert(view.cdg_snapshot.chunk_length == sizeof(snapshot_bytes));
    assert(memcmp(view.cdg_snapshot.snapshot_bytes, snapshot_bytes, sizeof(snapshot_bytes)) == 0);
}

static void test_protocol_v4_roundtrip(void) {
    uint8_t buffer[DASHCDG_MAX_PACKET_SIZE];
    struct dashcdg_packet_header header;
    struct dashcdg_packet_view view;
    struct dashcdg_v4_session_info_payload session_info;
    struct dashcdg_v4_loading_screen_payload loading_screen;
    struct dashcdg_v4_video_anchor_payload video_anchor;
    struct dashcdg_v4_audio_chunk_payload audio_chunk;
    struct dashcdg_v4_video_delta_payload video_delta;
    struct dashcdg_v4_repair_window_payload repair_window;
    struct dashcdg_v4_backfill_chunk_payload backfill_chunk;
    struct dashcdg_v4_clock_sync_payload clock_sync;
    struct dashcdg_v4_rx_stats_payload rx_stats;
    uint8_t anchor_bytes[] = { 0x01, 0x02, 0x03, 0x04 };
    uint8_t audio_bytes[] = { 0x11, 0x22, 0x33, 0x44 };
    uint8_t delta_bytes[] = { 0x21, 0x22, 0x23, 0x24, 0x25 };
    uint8_t repair_bytes[] = { 0x31, 0x32, 0x33 };
    uint8_t backfill_bytes[] = { 0x41, 0x42, 0x43, 0x44 };
    size_t size;

    memset(&header, 0, sizeof(header));
    header.sequence = 77;
    header.sender_time_ms = 9999;

    memset(&session_info, 0, sizeof(session_info));
    strcpy(session_info.song_id, "badnet-song");
    session_info.transport_version = DASHCDG_PROTOCOL_VERSION_V4;
    session_info.audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
    session_info.video_profile_id = 1;
    session_info.audio_codec_id = DASHCDG_V4_AUDIO_CODEC_SBC_LIKE;
    session_info.audio_sample_rate = 16000;
    session_info.audio_channels = 1;
    session_info.audio_frame_ms = 20;
    session_info.audio_bitrate_or_mode = 24;
    session_info.startup_preroll_ms = 240;
    session_info.audio_join_redundancy = 3;
    session_info.repair_mode = DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY;
    session_info.video_anchor_mode = DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS;
    session_info.video_delta_mode = DASHCDG_V4_VIDEO_DELTA_MODE_REPEAT_RUN;
    session_info.startup_backfill_mode = 1;
    session_info.loading_screen_mode = DASHCDG_V4_LOADING_SCREEN_CONNECTING;
    session_info.asset_size = 8192;
    session_info.session_start_ms = 4567;
    size = dashcdg_protocol_serialize_v4_session_info(buffer, sizeof(buffer), &header, &session_info);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.header.version == DASHCDG_PROTOCOL_VERSION_V4);
    assert(strcmp(view.v4_session_info.song_id, "badnet-song") == 0);
    assert(view.v4_session_info.audio_codec_id == DASHCDG_V4_AUDIO_CODEC_SBC_LIKE);
    assert(view.v4_session_info.startup_preroll_ms == 240);
    assert(dashcdg_v4_audio_codec_is_nb_ima_payload(view.v4_session_info.audio_codec_id) == 1);

    {
        const uint8_t codec_ids[] = {
                DASHCDG_V4_AUDIO_CODEC_CELP13K,
                DASHCDG_V4_AUDIO_CODEC_EVRC,
                DASHCDG_V4_AUDIO_CODEC_AMR_NB,
                DASHCDG_V4_AUDIO_CODEC_AMR_WB,
                DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC
        };
        size_t codec_index;

        for (codec_index = 0; codec_index < sizeof(codec_ids) / sizeof(codec_ids[0]); ++codec_index) {
            memset(&session_info, 0, sizeof(session_info));
            strcpy(session_info.song_id, "codec-scan");
            session_info.transport_version = DASHCDG_PROTOCOL_VERSION_V4;
            session_info.audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_RESILIENCE;
            session_info.video_profile_id = 1;
            session_info.audio_codec_id = codec_ids[codec_index];
            session_info.audio_sample_rate = 48000;
            session_info.audio_channels = 1;
            session_info.audio_frame_ms = 20;
            session_info.audio_bitrate_or_mode = 24;
            session_info.startup_preroll_ms = 240;
            session_info.audio_join_redundancy = 2;
            session_info.repair_mode = DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY;
            session_info.video_anchor_mode = DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS;
            session_info.video_delta_mode = DASHCDG_V4_VIDEO_DELTA_MODE_REPEAT_RUN;
            session_info.startup_backfill_mode = 1;
            session_info.loading_screen_mode = DASHCDG_V4_LOADING_SCREEN_CONNECTING;
            session_info.asset_size = 4096;
            session_info.session_start_ms = 9001;
            size = dashcdg_protocol_serialize_v4_session_info(buffer, sizeof(buffer), &header, &session_info);
            assert(size > 0);
            assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
            assert(strcmp(view.v4_session_info.song_id, "codec-scan") == 0);
            assert(view.v4_session_info.audio_codec_id == codec_ids[codec_index]);
            assert(dashcdg_v4_audio_codec_is_narrowband(view.v4_session_info.audio_codec_id) == 1);
            assert(dashcdg_v4_audio_codec_is_nb_ima_payload(view.v4_session_info.audio_codec_id) == 0);
        }
    }

    memset(&loading_screen, 0, sizeof(loading_screen));
    loading_screen.screen_id = 7;
    loading_screen.screen_kind = DASHCDG_V4_LOADING_SCREEN_LATE_JOIN;
    loading_screen.animation_phase = 2;
    loading_screen.anchor_packet_index = 321;
    strcpy(loading_screen.primary_text, "RECONNECT");
    size = dashcdg_protocol_serialize_v4_loading_screen(buffer, sizeof(buffer), &header, &loading_screen);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.v4_loading_screen.screen_kind == DASHCDG_V4_LOADING_SCREEN_LATE_JOIN);
    assert(view.v4_loading_screen.anchor_packet_index == 321);
    assert(strcmp(view.v4_loading_screen.primary_text, "RECONNECT") == 0);

    memset(&video_anchor, 0, sizeof(video_anchor));
    video_anchor.anchor_id = 5;
    video_anchor.anchor_format = DASHCDG_V4_VIDEO_ANCHOR_MODE_RLE_CANVAS;
    video_anchor.packet_index = 654;
    video_anchor.total_bytes = 64;
    video_anchor.anchor_offset = 4;
    video_anchor.chunk_length = sizeof(anchor_bytes);
    video_anchor.anchor_bytes = anchor_bytes;
    size = dashcdg_protocol_serialize_v4_video_anchor(buffer, sizeof(buffer), &header, &video_anchor);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.v4_video_anchor.anchor_id == 5);
    assert(view.v4_video_anchor.packet_index == 654);
    assert(memcmp(view.v4_video_anchor.anchor_bytes, anchor_bytes, sizeof(anchor_bytes)) == 0);

    memset(&audio_chunk, 0, sizeof(audio_chunk));
    audio_chunk.media_sequence = 12;
    audio_chunk.group_id = 4;
    audio_chunk.group_index = 1;
    audio_chunk.frame_ms = 20;
    audio_chunk.audio_profile_id = DASHCDG_V4_AUDIO_PROFILE_QUALITY;
    audio_chunk.codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
    audio_chunk.chunk_flags = 1;
    audio_chunk.playback_ms = 777;
    audio_chunk.encoded_length = sizeof(audio_bytes);
    audio_chunk.encoded_bytes = audio_bytes;
    size = dashcdg_protocol_serialize_v4_audio_chunk(buffer, sizeof(buffer), &header, &audio_chunk);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.v4_audio_chunk.media_sequence == 12);
    assert(view.v4_audio_chunk.codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS);
    assert(memcmp(view.v4_audio_chunk.encoded_bytes, audio_bytes, sizeof(audio_bytes)) == 0);

    memset(&video_delta, 0, sizeof(video_delta));
    video_delta.media_sequence = 33;
    video_delta.group_id = 6;
    video_delta.group_index = 0;
    video_delta.delta_format = DASHCDG_V4_VIDEO_DELTA_MODE_REPEAT_RUN;
    video_delta.delta_flags = 1;
    video_delta.packet_count = 3;
    video_delta.packet_start_index = 900;
    video_delta.encoded_length = sizeof(delta_bytes);
    video_delta.delta_bytes = delta_bytes;
    size = dashcdg_protocol_serialize_v4_video_delta(buffer, sizeof(buffer), &header, &video_delta);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.v4_video_delta.delta_format == DASHCDG_V4_VIDEO_DELTA_MODE_REPEAT_RUN);
    assert(view.v4_video_delta.packet_start_index == 900);
    assert(memcmp(view.v4_video_delta.delta_bytes, delta_bytes, sizeof(delta_bytes)) == 0);

    memset(&repair_window, 0, sizeof(repair_window));
    repair_window.stream_type = DASHCDG_STREAM_TYPE_AUDIO;
    repair_window.repair_mode = DASHCDG_V4_REPAIR_MODE_XOR_PLUS_STARTUP_REDUNDANCY;
    repair_window.redundancy_index = 1;
    repair_window.group_size = 4;
    repair_window.group_id = 88;
    repair_window.payload_length = sizeof(repair_bytes);
    repair_window.payload_bytes = repair_bytes;
    size = dashcdg_protocol_serialize_v4_repair_window(buffer, sizeof(buffer), &header, &repair_window);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.v4_repair_window.group_id == 88);
    assert(view.v4_repair_window.redundancy_index == 1);
    assert(memcmp(view.v4_repair_window.payload_bytes, repair_bytes, sizeof(repair_bytes)) == 0);

    memset(&backfill_chunk, 0, sizeof(backfill_chunk));
    backfill_chunk.asset_offset = 1024;
    backfill_chunk.chunk_length = sizeof(backfill_bytes);
    backfill_chunk.backfill_mode = 1;
    backfill_chunk.chunk_bytes = backfill_bytes;
    size = dashcdg_protocol_serialize_v4_backfill_chunk(buffer, sizeof(buffer), &header, &backfill_chunk);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.v4_backfill_chunk.asset_offset == 1024);
    assert(memcmp(view.v4_backfill_chunk.chunk_bytes, backfill_bytes, sizeof(backfill_bytes)) == 0);

    memset(&clock_sync, 0, sizeof(clock_sync));
    clock_sync.session_start_ms = 4567;
    clock_sync.playback_ms = 1234;
    clock_sync.startup_state = 2;
    size = dashcdg_protocol_serialize_v4_clock_sync(buffer, sizeof(buffer), &header, &clock_sync);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.v4_clock_sync.session_start_ms == 4567);
    assert(view.v4_clock_sync.playback_ms == 1234);
    assert(view.v4_clock_sync.startup_state == 2);

    assert(sizeof(struct dashcdg_v4_rx_stats_payload) == DASHCDG_V4_RX_STATS_PAYLOAD_V2_SIZE);

    memset(&rx_stats, 0, sizeof(rx_stats));
    rx_stats.report_seq = 3;
    rx_stats.wall_now_ms = 10002;
    rx_stats.sender_time_observed_ms = 10050;
    rx_stats.clock_offset_estimate_ms = -12;
    rx_stats.playout_delay_ms_config = 500;
    rx_stats.audio_buffer_ms = 120;
    rx_stats.audio_queue_pressure_events = 2;
    rx_stats.fec_audio_recovered = 9;
    rx_stats.jitter_rms_ms = 18;
    rx_stats.loss_pct_x100 = 150;
    rx_stats.v4_codec_id = DASHCDG_V4_AUDIO_CODEC_OPUS;
    rx_stats.opus_bitrate_bps = 96000;
    rx_stats.fec_decode_attempts = 100;
    rx_stats.fec_recovery_failed = 7;
    rx_stats.media_datagrams_lost_estimated = 3;
    rx_stats.cdg_fec_recovered = 11;
    rx_stats.cdg_fec_failed = 1;
    rx_stats.jitter_p95_ms = 42;
    rx_stats.jitter_max_ms = 99;
    rx_stats.reorder_events = 5;
    rx_stats.receiver_instance_id = 0xDEADBEEFU;
    rx_stats.fec_group_size_observed = 8;
    size = dashcdg_protocol_serialize_v4_rx_stats(buffer, sizeof(buffer), &header, &rx_stats);
    assert(size == DASHCDG_PACKET_HEADER_SIZE + DASHCDG_V4_RX_STATS_PAYLOAD_V2_SIZE);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(view.header.type == DASHCDG_PACKET_V4_RX_STATS);
    assert(view.header.payload_length == DASHCDG_V4_RX_STATS_PAYLOAD_V2_SIZE);
    assert(view.v4_rx_stats.report_seq == 3);
    assert(view.v4_rx_stats.wall_now_ms == 10002);
    assert(view.v4_rx_stats.sender_time_observed_ms == 10050);
    assert(view.v4_rx_stats.clock_offset_estimate_ms == -12);
    assert(view.v4_rx_stats.playout_delay_ms_config == 500);
    assert(view.v4_rx_stats.audio_buffer_ms == 120);
    assert(view.v4_rx_stats.audio_queue_pressure_events == 2);
    assert(view.v4_rx_stats.fec_audio_recovered == 9);
    assert(view.v4_rx_stats.jitter_rms_ms == 18);
    assert(view.v4_rx_stats.loss_pct_x100 == 150);
    assert(view.v4_rx_stats.v4_codec_id == DASHCDG_V4_AUDIO_CODEC_OPUS);
    assert(view.v4_rx_stats.opus_bitrate_bps == 96000);
    assert(view.v4_rx_stats.fec_decode_attempts == 100);
    assert(view.v4_rx_stats.fec_recovery_failed == 7);
    assert(view.v4_rx_stats.media_datagrams_lost_estimated == 3);
    assert(view.v4_rx_stats.cdg_fec_recovered == 11);
    assert(view.v4_rx_stats.cdg_fec_failed == 1);
    assert(view.v4_rx_stats.jitter_p95_ms == 42);
    assert(view.v4_rx_stats.jitter_max_ms == 99);
    assert(view.v4_rx_stats.reorder_events == 5);
    assert(view.v4_rx_stats.receiver_instance_id == 0xDEADBEEFU);
    assert(view.v4_rx_stats.fec_group_size_observed == 8);

    /* v1 wire (52-byte body): truncate v2 packet and fix header payload_length. */
    {
        uint8_t v1buf[256];

        memcpy(v1buf, buffer, DASHCDG_PACKET_HEADER_SIZE + DASHCDG_V4_RX_STATS_PAYLOAD_V1_SIZE);
        v1buf[20] = (uint8_t) ((DASHCDG_V4_RX_STATS_PAYLOAD_V1_SIZE >> 8U) & 0xFFU);
        v1buf[21] = (uint8_t) (DASHCDG_V4_RX_STATS_PAYLOAD_V1_SIZE & 0xFFU);
        memset(&view, 0, sizeof(view));
        assert(
                dashcdg_protocol_parse_packet(
                        &view,
                        v1buf,
                        DASHCDG_PACKET_HEADER_SIZE + DASHCDG_V4_RX_STATS_PAYLOAD_V1_SIZE
                ) == 1
        );
        assert(view.v4_rx_stats.opus_bitrate_bps == 96000);
        assert(view.v4_rx_stats.fec_decode_attempts == 0);
        assert(view.v4_rx_stats.receiver_instance_id == 0);
    }
}

static void test_media_clock(void) {
    struct dashcdg_media_clock clock_state;

    dashcdg_media_clock_init(&clock_state);
    dashcdg_media_clock_observe(&clock_state, 1000, 1200, 50);
    assert(clock_state.offset_ms == 200);
    dashcdg_media_clock_observe(&clock_state, 1010, 1500, 25);
    assert(clock_state.offset_ms == 225);
    assert(dashcdg_media_clock_remote_now(&clock_state, 2000) == 2225);

    dashcdg_media_clock_init(&clock_state);
    dashcdg_media_clock_observe_ptp_exchange(&clock_state, 1000, 905, 1200, 1315, 10, 10);
    assert(clock_state.offset_ms == 105);
    assert(clock_state.path_delay_ms == 10);

    dashcdg_media_clock_observe_ptp_exchange(&clock_state, 2000, 1908, 2200, 2316, 4, 3);
    assert(clock_state.offset_ms == 104);
    assert(clock_state.path_delay_ms == 12);
}

static void test_audio_jitter_duplicate_drop(void) {
    struct dashcdg_audio_jitter_buffer jb;
    const uint8_t pl[] = {0xab, 0xcd};
    uint64_t drops_before;

    dashcdg_audio_jitter_init(&jb);
    assert(dashcdg_audio_jitter_insert(&jb, 100U, 5000U, 20U, 0U, 0U, pl, (uint16_t) sizeof(pl), 1) == 1);
    assert(dashcdg_audio_jitter_occupied_count(&jb) == 1U);
    drops_before = jb.pending_drops;
    assert(dashcdg_audio_jitter_insert(&jb, 100U, 5000U, 20U, 0U, 0U, pl, (uint16_t) sizeof(pl), 1) == 0);
    assert(jb.pending_drops > drops_before);
}

static void test_audio_jitter_drain_apply_and_note(void) {
    struct dashcdg_audio_jitter_buffer jb;
    struct dashcdg_audio_jitter_frame *frame = NULL;
    struct dashcdg_audio_jitter_drain_input din;
    uint64_t miss = 0U;
    const uint8_t pl[] = {0x77};
    enum dashcdg_audio_drain_step step;

    dashcdg_audio_jitter_init(&jb);
    assert(dashcdg_audio_jitter_insert(&jb, 1U, 100U, 20U, 3U, 2U, pl, sizeof(pl), 0) == 1);

    memset(&din, 0, sizeof(din));
    step = dashcdg_audio_jitter_drain_step(&jb, &din, &frame, &miss);
    assert(step == DASHCDG_AUDIO_DRAIN_APPLY);
    assert(frame != NULL);
    assert(frame->media_sequence == 1U);
    assert(frame->encoded_length == 1U);
    assert(frame->encoded_bytes[0] == 0x77U);

    dashcdg_audio_jitter_note_applied(&jb, frame, 20U);
    assert(jb.next_media_sequence == 2U);
    assert(dashcdg_audio_jitter_occupied_count(&jb) == 0U);
}

static void test_audio_jitter_drain_skip_missing(void) {
    struct dashcdg_audio_jitter_buffer jb;
    struct dashcdg_audio_jitter_frame *frame = NULL;
    struct dashcdg_audio_jitter_drain_input din;
    uint64_t miss = 0U;
    const uint8_t one[] = {0x31};

    dashcdg_audio_jitter_init(&jb);
    assert(dashcdg_audio_jitter_insert(&jb, 10U, 2000U, 20U, 0U, 0U, one, sizeof(one), 0) == 1);

    memset(&din, 0, sizeof(din));
    din.have_sender_playback = 1;
    din.sender_playback_now_ms = 9000U;
    din.announced_audio_frame_ms = 20U;
    din.announced_playout_delay_ms = 80U;
    din.late_grace_ms = 0U;
    din.audio_stream_started = 1;
    din.audio_buffered_ms = 100U;

    assert(dashcdg_audio_jitter_drain_step(&jb, &din, &frame, &miss) == DASHCDG_AUDIO_DRAIN_APPLY);
    dashcdg_audio_jitter_note_applied(&jb, frame, 20U);

    miss = 0U;
    frame = NULL;
    assert(dashcdg_audio_jitter_drain_step(&jb, &din, &frame, &miss) == DASHCDG_AUDIO_DRAIN_SKIP);
    assert(miss == 1U);
    assert(jb.next_media_sequence == 12U);
}

static void test_audio_jitter_drain_no_ghost_skip_small_clock_skew(void) {
    struct dashcdg_audio_jitter_buffer jb;
    struct dashcdg_audio_jitter_frame *frame = NULL;
    struct dashcdg_audio_jitter_drain_input din;
    uint64_t miss = 0U;
    const uint8_t one[] = {0x31};

    dashcdg_audio_jitter_init(&jb);
    assert(dashcdg_audio_jitter_insert(&jb, 10U, 2000U, 20U, 0U, 0U, one, sizeof(one), 0) == 1);

    memset(&din, 0, sizeof(din));
    din.have_sender_playback = 1;
    /* After first apply, next expected is 11 at ~2020 ms; clock says 3600 => skew 1580 < 2000: do not ghost-skip. */
    din.sender_playback_now_ms = 3600U;
    din.announced_audio_frame_ms = 20U;
    din.announced_playout_delay_ms = 500U;
    din.late_grace_ms = 80U;
    din.audio_stream_started = 1;
    din.audio_buffered_ms = 300U;

    assert(dashcdg_audio_jitter_drain_step(&jb, &din, &frame, &miss) == DASHCDG_AUDIO_DRAIN_APPLY);
    dashcdg_audio_jitter_note_applied(&jb, frame, 20U);

    miss = 0U;
    frame = NULL;
    assert(dashcdg_audio_jitter_drain_step(&jb, &din, &frame, &miss) == DASHCDG_AUDIO_DRAIN_STOP);
    assert(jb.next_media_sequence == 11U);
}

static void test_audio_jitter_drain_empty_hole_recovery_bounded_skew(void) {
    struct dashcdg_audio_jitter_buffer jb;
    struct dashcdg_audio_jitter_frame *frame = NULL;
    struct dashcdg_audio_jitter_drain_input din;
    uint64_t miss = 0U;
    const uint8_t one[] = {0x31};

    dashcdg_audio_jitter_init(&jb);
    assert(dashcdg_audio_jitter_insert(&jb, 10U, 2000U, 20U, 0U, 0U, one, sizeof(one), 0) == 1);

    memset(&din, 0, sizeof(din));
    din.have_sender_playback = 1;
    din.announced_audio_frame_ms = 20U;
    din.announced_playout_delay_ms = 500U;
    din.late_grace_ms = 80U;
    din.audio_stream_started = 1;
    din.audio_buffered_ms = 300U;

    assert(dashcdg_audio_jitter_drain_step(&jb, &din, &frame, &miss) == DASHCDG_AUDIO_DRAIN_APPLY);
    dashcdg_audio_jitter_note_applied(&jb, frame, 20U);

    miss = 0U;
    frame = NULL;
    /* next_playback ~2020; sender 2320 => skew 300; 200ms since apply => ratio allows hole threshold. */
    din.sender_playback_now_ms = 2320U;
    din.ms_since_prior_audio_apply = 200U;
    assert(dashcdg_audio_jitter_drain_step(&jb, &din, &frame, &miss) == DASHCDG_AUDIO_DRAIN_SKIP);
    assert(miss == 1U);
    assert(jb.next_media_sequence == 12U);
}

static void test_cdg_raster_rgba_matches_memory_preset(void) {
    struct dashcdg_cdg_state state;
    struct dashcdg_subchannel_packet pkt = make_packet(DASHCDG_INSN_MEMORY_PRESET);
    uint8_t rgba[DASHCDG_CDG_RGBA_BYTES];

    dashcdg_cdg_state_init(&state);
    state.color_table[4] = 0x102030;
    state.transparency[4] = 0U;
    ((struct dashcdg_insn_memory_preset *) pkt.data)->color = 4;
    ((struct dashcdg_insn_memory_preset *) pkt.data)->repeat = 0;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);

    dashcdg_cdg_state_to_rgba8(&state, rgba);
    assert(rgba[0] == 0x10U);
    assert(rgba[1] == 0x20U);
    assert(rgba[2] == 0x30U);
    assert(rgba[3] == 255U);
}

static void test_cdg_batch_jitter_duplicate_drop(void) {
    struct dashcdg_cdg_batch_jitter_buffer jb;
    uint8_t one_pkt[DASHCDG_SUBCHANNEL_PACKET_BYTES];
    uint64_t drops_before;

    memset(one_pkt, 0, sizeof(one_pkt));
    dashcdg_cdg_batch_jitter_init(&jb);
    assert(dashcdg_cdg_batch_jitter_insert(&jb, 200U, 1U, one_pkt, 1) == 1);
    assert(dashcdg_cdg_batch_jitter_occupied_count(&jb) == 1U);
    drops_before = jb.pending_drops;
    assert(dashcdg_cdg_batch_jitter_insert(&jb, 200U, 1U, one_pkt, 1) == 0);
    assert(jb.pending_drops > drops_before);
}

static void test_cdg_batch_jitter_apply_note_and_drain_skip(void) {
    struct dashcdg_cdg_batch_jitter_buffer jb;
    struct dashcdg_cdg_batch_jitter_frame *batch = NULL;
    struct dashcdg_cdg_batch_jitter_drain_input din;
    uint8_t one_pkt[DASHCDG_SUBCHANNEL_PACKET_BYTES];
    uint64_t miss = 0U;
    enum dashcdg_cdg_batch_drain_step step;

    memset(one_pkt, 0x55, sizeof(one_pkt));
    dashcdg_cdg_batch_jitter_init(&jb);
    assert(dashcdg_cdg_batch_jitter_insert(&jb, 10U, 1U, one_pkt, 0) == 1);

    memset(&din, 0, sizeof(din));
    step = dashcdg_cdg_batch_jitter_drain_step(&jb, &din, &batch, &miss);
    assert(step == DASHCDG_CDG_BATCH_DRAIN_APPLY);
    assert(batch != NULL && batch->packet_start_index == 10U && batch->packet_count == 1U);
    dashcdg_cdg_batch_jitter_note_applied(&jb, batch);
    assert(jb.next_packet_index == 11U);

    miss = 0U;
    batch = NULL;
    memset(&din, 0, sizeof(din));
    din.have_sender_playback = 1;
    din.sender_playback_now_ms = 9000U;
    din.late_grace_ms = 0U;
    din.late_gate = 1;
    step = dashcdg_cdg_batch_jitter_drain_step(&jb, &din, &batch, &miss);
    assert(step == DASHCDG_CDG_BATCH_DRAIN_SKIP);
    assert(miss == 1U);
    assert(jb.next_packet_index == 11U + (uint64_t) DASHCDG_MAX_CDG_BATCH_PACKETS);
}

static void test_cdg_batch_jitter_snapshot_seek_purges_old_slots(void) {
    struct dashcdg_cdg_batch_jitter_buffer jb;
    uint8_t one_pkt[DASHCDG_SUBCHANNEL_PACKET_BYTES];

    memset(one_pkt, 0x11, sizeof(one_pkt));
    dashcdg_cdg_batch_jitter_init(&jb);
    assert(dashcdg_cdg_batch_jitter_insert(&jb, 0U, 1U, one_pkt, 0) == 1);
    assert(dashcdg_cdg_batch_jitter_insert(&jb, 24U, 1U, one_pkt, 0) == 1);
    assert(dashcdg_cdg_batch_jitter_occupied_count(&jb) == 2U);

    dashcdg_cdg_batch_jitter_apply_snapshot_seek(&jb, 30U);
    assert(jb.next_packet_index == 30U);
    assert(dashcdg_cdg_batch_jitter_occupied_count(&jb) == 0U);
}

static void test_cdg_raster_alpha_from_transparency(void) {
    struct dashcdg_cdg_state state;
    struct dashcdg_subchannel_packet pkt = make_packet(DASHCDG_INSN_MEMORY_PRESET);
    uint8_t rgba[DASHCDG_CDG_RGBA_BYTES];

    dashcdg_cdg_state_init(&state);
    state.color_table[6] = 0x00aabb;
    state.transparency[6] = 63U;
    ((struct dashcdg_insn_memory_preset *) pkt.data)->color = 6;
    ((struct dashcdg_insn_memory_preset *) pkt.data)->repeat = 0;
    assert(dashcdg_cdg_state_process_packet(&state, &pkt) == 1);

    dashcdg_cdg_state_to_rgba8(&state, rgba);
    assert(rgba[0] == 0x00U);
    assert(rgba[1] == 0xaaU);
    assert(rgba[2] == 0xbbU);
    assert(rgba[3] == 0U);
}

static void test_fec_recovery(void) {
    struct dashcdg_fec_parity_state parity;
    const uint8_t *known_payloads[2];
    uint16_t known_lengths[2];
    uint8_t recovered[DASHCDG_MAX_FEC_PAYLOAD_BYTES];
    uint16_t recovered_length = 0;
    const uint8_t payload_a[] = { 0x10, 0x20, 0x30, 0x40 };
    const uint8_t payload_b[] = { 0x55, 0x66, 0x77 };
    const uint8_t payload_c[] = { 0x99, 0xaa, 0xbb, 0xcc, 0xdd };

    dashcdg_fec_parity_init(&parity);
    assert(dashcdg_fec_parity_accumulate(&parity, payload_a, sizeof(payload_a)) == 1);
    assert(dashcdg_fec_parity_accumulate(&parity, payload_b, sizeof(payload_b)) == 1);
    assert(dashcdg_fec_parity_accumulate(&parity, payload_c, sizeof(payload_c)) == 1);

    known_payloads[0] = payload_a;
    known_lengths[0] = sizeof(payload_a);
    known_payloads[1] = payload_c;
    known_lengths[1] = sizeof(payload_c);
    assert(dashcdg_fec_parity_recover(&parity, known_payloads, known_lengths, 2, recovered, &recovered_length) == 1);
    assert(recovered_length == sizeof(payload_b));
    assert(memcmp(recovered, payload_b, sizeof(payload_b)) == 0);
}

static void test_v4_audio_codec_predicate_helpers(void) {
    assert(dashcdg_v4_audio_codec_is_evrc(DASHCDG_V4_AUDIO_CODEC_EVRC) == 1);
    assert(dashcdg_v4_audio_codec_is_evrc(DASHCDG_V4_AUDIO_CODEC_OPUS) == 0);
    assert(dashcdg_v4_audio_codec_is_qcelp13k(DASHCDG_V4_AUDIO_CODEC_CELP13K) == 1);
    assert(dashcdg_v4_audio_codec_is_qcelp13k(DASHCDG_V4_AUDIO_CODEC_SBC_LIKE) == 0);
    assert(dashcdg_v4_audio_codec_is_bluetooth_sbc(DASHCDG_V4_AUDIO_CODEC_BLUETOOTH_SBC) == 1);
    assert(dashcdg_v4_audio_codec_is_bluetooth_sbc(DASHCDG_V4_AUDIO_CODEC_AMR_WB) == 0);
}

static void test_nb_ima_codec_roundtrip(void) {
    struct dashcdg_nb_ima_state enc;
    struct dashcdg_nb_ima_state dec;
    int16_t pcm[DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES];
    int16_t out[DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES];
    uint8_t pkt[DASHCDG_NB_IMA_ENCODED_BYTES];
    int enc_len;
    int dec_len;
    size_t i;
    int32_t energy = 0;

    for (i = 0; i < DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES; ++i) {
        pcm[i] = (int16_t) (400 * (int) ((i / 40) % 3) - 400);
    }
    dashcdg_nb_ima_state_init(&enc);
    enc_len = dashcdg_nb_ima_encode_pcm48_mono_frame(&enc, pcm, DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES, pkt, sizeof(pkt));
    assert(enc_len == (int) DASHCDG_NB_IMA_ENCODED_BYTES);

    dashcdg_nb_ima_state_init(&dec);
    dec_len = dashcdg_nb_ima_decode_to_pcm48_mono_frame(&dec, pkt, (size_t) enc_len, out, DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES);
    assert(dec_len == (int) DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES);

    for (i = 0; i < DASHCDG_NB_IMA_PCM48_INPUT_SAMPLES; ++i) {
        int32_t d = (int32_t) out[i];
        energy += d * d;
    }
    assert(energy > 1000);
}

int main(void) {
    test_memory_and_border();
    test_tile_and_scroll_copy();
    test_scroll_preset_and_transparency();
    test_scroll_copy_direction_and_offset_clamp();
    test_reader_seek_and_keyframes();
    test_protocol_roundtrip();
    test_protocol_v4_roundtrip();
    test_v4_audio_codec_predicate_helpers();
    test_media_clock();
    test_audio_jitter_duplicate_drop();
    test_audio_jitter_drain_apply_and_note();
    test_audio_jitter_drain_skip_missing();
    test_audio_jitter_drain_no_ghost_skip_small_clock_skew();
    test_audio_jitter_drain_empty_hole_recovery_bounded_skew();
    test_cdg_raster_rgba_matches_memory_preset();
    test_cdg_raster_alpha_from_transparency();
    test_cdg_batch_jitter_duplicate_drop();
    test_cdg_batch_jitter_apply_note_and_drain_skip();
    test_cdg_batch_jitter_snapshot_seek_purges_old_slots();
    test_fec_recovery();
    test_nb_ima_codec_roundtrip();

    puts("all tests passed");
    return 0;
}
