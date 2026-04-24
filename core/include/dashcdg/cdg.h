#ifndef DASHCDG_CDG_H
#define DASHCDG_CDG_H

#include <stddef.h>
#include <stdint.h>

#include "dashcdg/common.h"

enum {
    DASHCDG_INSN_INVALID = -2,
    DASHCDG_INSN_UNKNOWN = -1,
    DASHCDG_INSN_MEMORY_PRESET = 1,
    DASHCDG_INSN_BORDER_PRESET = 2,
    DASHCDG_INSN_TILE_BLOCK = 6,
    DASHCDG_INSN_SCROLL_PRESET = 20,
    DASHCDG_INSN_SCROLL_COPY = 24,
    DASHCDG_INSN_DEF_TRANSPARENT = 28,
    DASHCDG_INSN_LOAD_COLOR_TABLE_00 = 30,
    DASHCDG_INSN_LOAD_COLOR_TABLE_08 = 31,
    DASHCDG_INSN_TILE_BLOCK_XOR = 38
};

#pragma pack(push, 1)
struct dashcdg_subchannel_packet {
    uint8_t command;
    uint8_t instruction;
    uint8_t parity_q[2];
    uint8_t data[16];
    uint8_t parity_p[4];
};

struct dashcdg_insn_memory_preset {
    uint8_t color;
    uint8_t repeat;
    uint8_t filler[14];
};

struct dashcdg_insn_border_preset {
    uint8_t color;
    uint8_t filler[15];
};

struct dashcdg_insn_tile_block {
    uint8_t color_0;
    uint8_t color_1;
    uint8_t row;
    uint8_t column;
    uint8_t pixels[12];
};

struct dashcdg_insn_scroll {
    uint8_t color;
    uint8_t h_scroll;
    uint8_t v_scroll;
    uint8_t filler[13];
};

struct dashcdg_insn_define_transparent {
    uint8_t transparent[16];
};

struct dashcdg_insn_load_color_table {
    uint16_t spec[8];
};
#pragma pack(pop)

struct dashcdg_cdg_keyframe {
    dashcdg_tick_t timestamp;
    int color_table[DASHCDG_COLORS];
    uint8_t clear_color;
    uint8_t transparency[DASHCDG_COLORS];
    uint8_t display_h_offset;
    uint8_t display_v_offset;
};

struct dashcdg_cdg_keyframe_list {
    size_t count;
    struct dashcdg_cdg_keyframe *items;
};

struct dashcdg_cdg_state {
    dashcdg_tick_t ts;
    int color_table[DASHCDG_COLORS];
    uint8_t framebuffer[DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT];
    uint8_t transparency[DASHCDG_COLORS];
    uint8_t display_h_offset;
    uint8_t display_v_offset;
    /** Raster dirty tracking (visible 288x192 output space, half-open [vx0,vx1)×[vy0,vy1)). */
    uint8_t raster_dirty_full;
    uint8_t raster_dirty_has_partial;
    int raster_d_vx0;
    int raster_d_vy0;
    int raster_d_vx1;
    int raster_d_vy1;
};

struct dashcdg_cdg_reader {
    int eof;
    uint8_t *buffer;
    size_t buffer_size;
    size_t buffer_index;
    struct dashcdg_cdg_state state;
    struct dashcdg_cdg_keyframe_list keyframes;
};

void dashcdg_cdg_state_init(struct dashcdg_cdg_state *state);
int dashcdg_cdg_state_process_packet(struct dashcdg_cdg_state *state, const struct dashcdg_subchannel_packet *pkt);

typedef enum {
    DASHCDG_CDG_RASTER_DIRTY_NONE = 0,
    DASHCDG_CDG_RASTER_DIRTY_PARTIAL,
    DASHCDG_CDG_RASTER_DIRTY_FULL,
} dashcdg_cdg_raster_dirty_kind_t;

/** Whole visible window must be re-rastered (palette, preset, scroll, etc.). */
void dashcdg_cdg_state_raster_dirty_mark_full(struct dashcdg_cdg_state *state);

/**
 * Consume accumulated dirty region for incremental RGB565 raster.
 * On FULL or PARTIAL, writes half-open rect into *vx0..*vy1 (visible coords 0..288, 0..192).
 */
dashcdg_cdg_raster_dirty_kind_t dashcdg_cdg_state_take_raster_dirty(struct dashcdg_cdg_state *state, int *vx0, int *vy0,
                                                                  int *vx1, int *vy1);

void dashcdg_cdg_reader_init(struct dashcdg_cdg_reader *reader);
void dashcdg_cdg_reader_free(struct dashcdg_cdg_reader *reader);
int dashcdg_cdg_reader_load_memory(struct dashcdg_cdg_reader *reader, const uint8_t *data, size_t size);
void dashcdg_cdg_reader_reset(struct dashcdg_cdg_reader *reader);
int dashcdg_cdg_reader_read_packet(struct dashcdg_cdg_reader *reader, struct dashcdg_subchannel_packet *out_pkt);
int dashcdg_cdg_reader_build_keyframes(struct dashcdg_cdg_reader *reader);
int dashcdg_cdg_reader_seek(struct dashcdg_cdg_reader *reader, dashcdg_tick_t ts);

#endif
