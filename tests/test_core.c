#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
#include "dashcdg/fec.h"
#include "dashcdg/media_clock.h"
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

int main(void) {
    test_memory_and_border();
    test_tile_and_scroll_copy();
    test_scroll_preset_and_transparency();
    test_reader_seek_and_keyframes();
    test_protocol_roundtrip();
    test_media_clock();
    test_fec_recovery();

    puts("all tests passed");
    return 0;
}
