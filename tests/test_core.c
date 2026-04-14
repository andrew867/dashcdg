#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "dashcdg/cdg.h"
#include "dashcdg/common.h"
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
    uint8_t chunk_bytes[] = { 1, 2, 3, 4 };
    size_t size;

    memset(&header, 0, sizeof(header));
    header.sequence = 42;
    header.sender_time_ms = 1234;

    memset(&announce, 0, sizeof(announce));
    strcpy(announce.song_id, "demo-song");
    announce.asset_size = 4096;
    announce.chunk_size = 1024;
    announce.packets_per_second = DASHCDG_PACKETS_PER_SECOND;
    announce.session_start_ms = 5000;

    size = dashcdg_protocol_serialize_announce(buffer, sizeof(buffer), &header, &announce);
    assert(size > 0);
    assert(dashcdg_protocol_parse_packet(&view, buffer, size) == 1);
    assert(strcmp(view.announce.song_id, "demo-song") == 0);
    assert(view.announce.asset_size == 4096);

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
}

static void test_media_clock(void) {
    struct dashcdg_media_clock clock_state;

    dashcdg_media_clock_init(&clock_state);
    dashcdg_media_clock_observe(&clock_state, 1000, 1200, 50);
    assert(clock_state.offset_ms == 200);
    dashcdg_media_clock_observe(&clock_state, 1010, 1500, 25);
    assert(clock_state.offset_ms == 225);
    assert(dashcdg_media_clock_remote_now(&clock_state, 2000) == 2225);
}

int main(void) {
    test_memory_and_border();
    test_tile_and_scroll_copy();
    test_scroll_preset_and_transparency();
    test_reader_seek_and_keyframes();
    test_protocol_roundtrip();
    test_media_clock();

    puts("all tests passed");
    return 0;
}
