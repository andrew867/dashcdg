#include "dashcdg/cdg.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint16_t dashcdg_be16_to_host(uint16_t value) {
    return (uint16_t) (((value & 0x00FFU) << 8U) | ((value & 0xFF00U) >> 8U));
}

void dashcdg_cdg_state_raster_dirty_mark_full(struct dashcdg_cdg_state *s)
{
    if (s == NULL) {
        return;
    }
    s->raster_dirty_full = 1;
    s->raster_dirty_has_partial = 0;
}

static void dashcdg_dirty_union_screen_rect(struct dashcdg_cdg_state *s, int sx0, int sy0, int sx1, int sy1)
{
    int hx;
    int vo;
    int vx0;
    int vy0;
    int vx1;
    int vy1;

    if (s == NULL || s->raster_dirty_full) {
        return;
    }
    if (sx0 < 0) {
        sx0 = 0;
    }
    if (sy0 < 0) {
        sy0 = 0;
    }
    if (sx1 > DASHCDG_SCREEN_WIDTH) {
        sx1 = DASHCDG_SCREEN_WIDTH;
    }
    if (sy1 > DASHCDG_SCREEN_HEIGHT) {
        sy1 = DASHCDG_SCREEN_HEIGHT;
    }
    if (sx0 >= sx1 || sy0 >= sy1) {
        return;
    }

    hx = (int)s->display_h_offset;
    if (hx >= DASHCDG_TILE_WIDTH) {
        hx = DASHCDG_TILE_WIDTH - 1;
    }
    vo = (int)s->display_v_offset;
    if (vo >= DASHCDG_TILE_HEIGHT) {
        vo = DASHCDG_TILE_HEIGHT - 1;
    }

    vx0 = sx0 - (int)DASHCDG_VISIBLE_X - hx;
    vy0 = sy0 - (int)DASHCDG_VISIBLE_Y - vo;
    vx1 = sx1 - (int)DASHCDG_VISIBLE_X - hx;
    vy1 = sy1 - (int)DASHCDG_VISIBLE_Y - vo;

    if (vx0 < 0) {
        vx0 = 0;
    }
    if (vy0 < 0) {
        vy0 = 0;
    }
    if (vx1 > DASHCDG_VISIBLE_WIDTH) {
        vx1 = DASHCDG_VISIBLE_WIDTH;
    }
    if (vy1 > DASHCDG_VISIBLE_HEIGHT) {
        vy1 = DASHCDG_VISIBLE_HEIGHT;
    }
    if (vx0 >= vx1 || vy0 >= vy1) {
        return;
    }

    if (!s->raster_dirty_has_partial) {
        s->raster_d_vx0 = vx0;
        s->raster_d_vy0 = vy0;
        s->raster_d_vx1 = vx1;
        s->raster_d_vy1 = vy1;
        s->raster_dirty_has_partial = 1;
    } else {
        if (vx0 < s->raster_d_vx0) {
            s->raster_d_vx0 = vx0;
        }
        if (vy0 < s->raster_d_vy0) {
            s->raster_d_vy0 = vy0;
        }
        if (vx1 > s->raster_d_vx1) {
            s->raster_d_vx1 = vx1;
        }
        if (vy1 > s->raster_d_vy1) {
            s->raster_d_vy1 = vy1;
        }
    }
}

dashcdg_cdg_raster_dirty_kind_t dashcdg_cdg_state_take_raster_dirty(struct dashcdg_cdg_state *s, int *vx0, int *vy0,
                                                                  int *vx1, int *vy1)
{
    if (s == NULL) {
        return DASHCDG_CDG_RASTER_DIRTY_NONE;
    }
    if (s->raster_dirty_full) {
        s->raster_dirty_full = 0;
        s->raster_dirty_has_partial = 0;
        if (vx0 && vy0 && vx1 && vy1) {
            *vx0 = 0;
            *vy0 = 0;
            *vx1 = DASHCDG_VISIBLE_WIDTH;
            *vy1 = DASHCDG_VISIBLE_HEIGHT;
        }
        return DASHCDG_CDG_RASTER_DIRTY_FULL;
    }
    if (s->raster_dirty_has_partial) {
        if (vx0 && vy0 && vx1 && vy1) {
            *vx0 = s->raster_d_vx0;
            *vy0 = s->raster_d_vy0;
            *vx1 = s->raster_d_vx1;
            *vy1 = s->raster_d_vy1;
        }
        s->raster_dirty_has_partial = 0;
        return DASHCDG_CDG_RASTER_DIRTY_PARTIAL;
    }
    return DASHCDG_CDG_RASTER_DIRTY_NONE;
}

static int dashcdg_color_to_rgb(uint16_t color) {
    int r = ((color & 0x3C00U) >> 10U) * 16;
    int g = (((color & 0x0300U) >> 6U) | ((color & 0x0030U) >> 4U)) * 16;
    int b = (color & 0x000FU) * 16;

    return (r << 16) | (g << 8) | b;
}

static void dashcdg_scroll_canvas(struct dashcdg_cdg_state *state, int dx, int dy, uint8_t fill_color, int wrap) {
    uint8_t original[DASHCDG_SCREEN_WIDTH * DASHCDG_SCREEN_HEIGHT];

    if (state == NULL) {
        return;
    }

    memcpy(original, state->framebuffer, sizeof(original));
    memset(state->framebuffer, fill_color & 0x0FU, sizeof(state->framebuffer));

    for (int y = 0; y < DASHCDG_SCREEN_HEIGHT; ++y) {
        int dest_y = y + dy;
        for (int x = 0; x < DASHCDG_SCREEN_WIDTH; ++x) {
            int dest_x = x + dx;

            if (wrap) {
                while (dest_x < 0) {
                    dest_x += DASHCDG_SCREEN_WIDTH;
                }
                while (dest_y < 0) {
                    dest_y += DASHCDG_SCREEN_HEIGHT;
                }
                dest_x %= DASHCDG_SCREEN_WIDTH;
                dest_y %= DASHCDG_SCREEN_HEIGHT;
            } else if (dest_x < 0 || dest_x >= DASHCDG_SCREEN_WIDTH || dest_y < 0 || dest_y >= DASHCDG_SCREEN_HEIGHT) {
                continue;
            }

            state->framebuffer[DASHCDG_ARRAY_INDEX(dest_x, dest_y)] = original[DASHCDG_ARRAY_INDEX(x, y)];
        }
    }
}

static void dashcdg_capture_keyframe(
        struct dashcdg_cdg_keyframe *keyframe,
        const struct dashcdg_cdg_state *state,
        dashcdg_tick_t timestamp,
        uint8_t clear_color
) {
    keyframe->timestamp = timestamp;
    keyframe->clear_color = clear_color & 0x0FU;
    keyframe->display_h_offset = state->display_h_offset;
    keyframe->display_v_offset = state->display_v_offset;
    memcpy(keyframe->color_table, state->color_table, sizeof(keyframe->color_table));
    memcpy(keyframe->transparency, state->transparency, sizeof(keyframe->transparency));
}

static void dashcdg_restore_keyframe(struct dashcdg_cdg_reader *reader, const struct dashcdg_cdg_keyframe *keyframe) {
    reader->state.ts = keyframe->timestamp;
    reader->buffer_index = (size_t) (keyframe->timestamp * sizeof(struct dashcdg_subchannel_packet));
    reader->state.display_h_offset = keyframe->display_h_offset;
    reader->state.display_v_offset = keyframe->display_v_offset;
    memcpy(reader->state.color_table, keyframe->color_table, sizeof(reader->state.color_table));
    memcpy(reader->state.transparency, keyframe->transparency, sizeof(reader->state.transparency));
    memset(reader->state.framebuffer, keyframe->clear_color, sizeof(reader->state.framebuffer));
}

static const struct dashcdg_cdg_keyframe *dashcdg_find_closest_keyframe(
        const struct dashcdg_cdg_keyframe_list *list,
        dashcdg_tick_t ts
) {
    size_t low;
    size_t high;
    size_t best_index;

    if (list == NULL || list->count == 0) {
        return NULL;
    }

    low = 0;
    high = list->count - 1;
    best_index = 0;

    while (low <= high) {
        size_t mid = low + ((high - low) / 2U);
        const struct dashcdg_cdg_keyframe *candidate = &list->items[mid];

        if (candidate->timestamp == ts) {
            return candidate;
        }

        if (candidate->timestamp < ts) {
            best_index = mid;
            low = mid + 1U;
        } else {
            if (mid == 0) {
                break;
            }
            high = mid - 1U;
        }
    }

    return &list->items[best_index];
}

void dashcdg_cdg_state_init(struct dashcdg_cdg_state *state) {
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

int dashcdg_cdg_state_process_packet(struct dashcdg_cdg_state *state, const struct dashcdg_subchannel_packet *pkt) {
    uint8_t instruction;
    const uint8_t *data;

    if (state == NULL || pkt == NULL) {
        return 0;
    }

    state->ts++;

    if ((pkt->command & 0x3FU) != 0x09U) {
        return 0;
    }

    instruction = pkt->instruction & 0x3FU;
    data = pkt->data;

    switch (instruction) {
        case DASHCDG_INSN_LOAD_COLOR_TABLE_00:
        case DASHCDG_INSN_LOAD_COLOR_TABLE_08: {
            const struct dashcdg_insn_load_color_table *table = (const struct dashcdg_insn_load_color_table *) data;
            size_t offset = instruction == DASHCDG_INSN_LOAD_COLOR_TABLE_00 ? 0U : 8U;

            for (size_t i = 0; i < 8U; ++i) {
                uint16_t spec = dashcdg_be16_to_host(table->spec[i]) & 0x3F3FU;
                state->color_table[offset + i] = dashcdg_color_to_rgb(spec);
            }

            dashcdg_cdg_state_raster_dirty_mark_full(state);
            return 1;
        }

        case DASHCDG_INSN_MEMORY_PRESET: {
            const struct dashcdg_insn_memory_preset *preset = (const struct dashcdg_insn_memory_preset *) data;

            if ((preset->repeat & 0x0FU) == 0U) {
                memset(state->framebuffer, preset->color & 0x0FU, sizeof(state->framebuffer));
                dashcdg_cdg_state_raster_dirty_mark_full(state);
                return 1;
            }

            return 0;
        }

        case DASHCDG_INSN_BORDER_PRESET: {
            const struct dashcdg_insn_border_preset *border = (const struct dashcdg_insn_border_preset *) data;
            uint8_t color = border->color & 0x0FU;

            for (int x = 0; x < DASHCDG_SCREEN_WIDTH; ++x) {
                for (int y = 0; y < DASHCDG_SCREEN_HEIGHT; ++y) {
                    if (x >= DASHCDG_VISIBLE_X && x < DASHCDG_VISIBLE_RIGHT &&
                            y >= DASHCDG_VISIBLE_Y && y < DASHCDG_VISIBLE_BOTTOM) {
                        continue;
                    }

                    state->framebuffer[DASHCDG_ARRAY_INDEX(x, y)] = color;
                }
            }

            dashcdg_cdg_state_raster_dirty_mark_full(state);
            return 1;
        }

        case DASHCDG_INSN_TILE_BLOCK:
        case DASHCDG_INSN_TILE_BLOCK_XOR: {
            const struct dashcdg_insn_tile_block *tile = (const struct dashcdg_insn_tile_block *) data;
            int is_xor = instruction == DASHCDG_INSN_TILE_BLOCK_XOR;
            int start_row = (tile->row & 0x1FU) * DASHCDG_TILE_HEIGHT;
            int start_col = (tile->column & 0x3FU) * DASHCDG_TILE_WIDTH;

            for (int row = 0; row < DASHCDG_TILE_HEIGHT; ++row) {
                uint8_t tile_pixels = tile->pixels[row] & 0x3FU;

                for (int col = 0; col < DASHCDG_TILE_WIDTH; ++col) {
                    int x = start_col + col;
                    int y = start_row + row;
                    uint8_t bit = (tile_pixels >> (5 - col)) & 0x01U;
                    uint8_t color = bit ? (tile->color_1 & 0x0FU) : (tile->color_0 & 0x0FU);

                    if (x < 0 || x >= DASHCDG_SCREEN_WIDTH || y < 0 || y >= DASHCDG_SCREEN_HEIGHT) {
                        continue;
                    }

                    if (is_xor) {
                        state->framebuffer[DASHCDG_ARRAY_INDEX(x, y)] ^= color;
                        state->framebuffer[DASHCDG_ARRAY_INDEX(x, y)] &= 0x0FU;
                    } else {
                        state->framebuffer[DASHCDG_ARRAY_INDEX(x, y)] = color;
                    }
                }
            }

            dashcdg_dirty_union_screen_rect(state, start_col, start_row, start_col + DASHCDG_TILE_WIDTH,
                                            start_row + DASHCDG_TILE_HEIGHT);
            return 1;
        }

        case DASHCDG_INSN_SCROLL_PRESET:
        case DASHCDG_INSN_SCROLL_COPY: {
            const struct dashcdg_insn_scroll *scroll = (const struct dashcdg_insn_scroll *) data;
            uint8_t fill_color = scroll->color & 0x0FU;
            uint8_t h_scroll = scroll->h_scroll & 0x3FU;
            uint8_t v_scroll = scroll->v_scroll & 0x3FU;
            int h_command = (h_scroll & 0x30U) >> 4U;
            int v_command = (v_scroll & 0x30U) >> 4U;
            int dx = 0;
            int dy = 0;

            state->display_h_offset = h_scroll & 0x07U;
            if (state->display_h_offset >= DASHCDG_TILE_WIDTH) {
                state->display_h_offset = DASHCDG_TILE_WIDTH - 1U;
            }
            state->display_v_offset = v_scroll & 0x0FU;
            if (state->display_v_offset >= DASHCDG_TILE_HEIGHT) {
                state->display_v_offset = DASHCDG_TILE_HEIGHT - 1U;
            }

            if (h_command == 1) {
                dx = DASHCDG_TILE_WIDTH;
            } else if (h_command == 2) {
                dx = -DASHCDG_TILE_WIDTH;
            }

            if (v_command == 1) {
                dy = DASHCDG_TILE_HEIGHT;
            } else if (v_command == 2) {
                dy = -DASHCDG_TILE_HEIGHT;
            }

            if (dx != 0 || dy != 0) {
                dashcdg_scroll_canvas(
                        state,
                        dx,
                        dy,
                        fill_color,
                        instruction == DASHCDG_INSN_SCROLL_COPY
                );
            }

            dashcdg_cdg_state_raster_dirty_mark_full(state);
            return 1;
        }

        case DASHCDG_INSN_DEF_TRANSPARENT: {
            const struct dashcdg_insn_define_transparent *transparent =
                    (const struct dashcdg_insn_define_transparent *) data;

            for (size_t i = 0; i < DASHCDG_COLORS; ++i) {
                state->transparency[i] = transparent->transparent[i] & 0x3FU;
            }

            dashcdg_cdg_state_raster_dirty_mark_full(state);
            return 1;
        }

        default:
            return 0;
    }
}

void dashcdg_cdg_reader_init(struct dashcdg_cdg_reader *reader) {
    if (reader == NULL) {
        return;
    }

    memset(reader, 0, sizeof(*reader));
    dashcdg_cdg_state_init(&reader->state);
}

void dashcdg_cdg_reader_free(struct dashcdg_cdg_reader *reader) {
    if (reader == NULL) {
        return;
    }

    free(reader->keyframes.items);
    reader->keyframes.items = NULL;
    reader->keyframes.count = 0;

    free(reader->buffer);
    reader->buffer = NULL;
    reader->buffer_size = 0;
    reader->buffer_index = 0;
}

int dashcdg_cdg_reader_load_memory(struct dashcdg_cdg_reader *reader, const uint8_t *data, size_t size) {
    if (reader == NULL || data == NULL || size == 0) {
        return 0;
    }

    dashcdg_cdg_reader_free(reader);
    dashcdg_cdg_reader_init(reader);

    reader->buffer = (uint8_t *) malloc(size);
    if (reader->buffer == NULL) {
        return 0;
    }

    memcpy(reader->buffer, data, size);
    reader->buffer_size = size;
    reader->buffer_index = 0;
    reader->eof = 0;
    return 1;
}

void dashcdg_cdg_reader_reset(struct dashcdg_cdg_reader *reader) {
    if (reader == NULL) {
        return;
    }

    reader->eof = 0;
    reader->buffer_index = 0;
    dashcdg_cdg_state_init(&reader->state);
}

int dashcdg_cdg_reader_read_packet(struct dashcdg_cdg_reader *reader, struct dashcdg_subchannel_packet *out_pkt) {
    size_t packet_size = sizeof(struct dashcdg_subchannel_packet);

    if (reader == NULL || out_pkt == NULL) {
        return 0;
    }

    if (reader->buffer_index + packet_size > reader->buffer_size) {
        reader->eof = 1;
        return 0;
    }

    memcpy(out_pkt, reader->buffer + reader->buffer_index, packet_size);
    reader->buffer_index += packet_size;
    return 1;
}

int dashcdg_cdg_reader_build_keyframes(struct dashcdg_cdg_reader *reader) {
    struct dashcdg_subchannel_packet pkt;

    if (reader == NULL || reader->buffer == NULL) {
        return 0;
    }

    free(reader->keyframes.items);
    reader->keyframes.items = NULL;
    reader->keyframes.count = 0;

    dashcdg_cdg_reader_reset(reader);

    while (dashcdg_cdg_reader_read_packet(reader, &pkt)) {
        uint8_t instruction = pkt.instruction & 0x3FU;
        int needs_keyframe = 0;
        uint8_t clear_color = 0;

        if ((pkt.command & 0x3FU) != 0x09U) {
            reader->state.ts++;
            continue;
        }

        if (instruction == DASHCDG_INSN_MEMORY_PRESET) {
            const struct dashcdg_insn_memory_preset *preset = (const struct dashcdg_insn_memory_preset *) pkt.data;

            if ((preset->repeat & 0x0FU) == 0U) {
                needs_keyframe = 1;
                clear_color = preset->color & 0x0FU;
            }
        }

        dashcdg_cdg_state_process_packet(&reader->state, &pkt);

        if (needs_keyframe) {
            size_t next_count = reader->keyframes.count + 1U;
            struct dashcdg_cdg_keyframe *items = (struct dashcdg_cdg_keyframe *) realloc(
                    reader->keyframes.items,
                    next_count * sizeof(*reader->keyframes.items)
            );

            if (items == NULL) {
                dashcdg_cdg_reader_reset(reader);
                return 0;
            }

            reader->keyframes.items = items;
            dashcdg_capture_keyframe(
                    &reader->keyframes.items[reader->keyframes.count],
                    &reader->state,
                    reader->state.ts,
                    clear_color
            );
            reader->keyframes.count = next_count;
        }
    }

    dashcdg_cdg_reader_reset(reader);
    return 1;
}

int dashcdg_cdg_reader_seek(struct dashcdg_cdg_reader *reader, dashcdg_tick_t ts) {
    struct dashcdg_subchannel_packet pkt;
    int needs_update = 0;

    if (reader == NULL) {
        return 0;
    }

    if (ts < reader->state.ts) {
        const struct dashcdg_cdg_keyframe *keyframe = dashcdg_find_closest_keyframe(&reader->keyframes, ts);

        if (keyframe != NULL) {
            dashcdg_restore_keyframe(reader, keyframe);
        } else {
            dashcdg_cdg_reader_reset(reader);
        }
    }

    while (reader->state.ts < ts) {
        if (!dashcdg_cdg_reader_read_packet(reader, &pkt)) {
            reader->eof = 1;
            break;
        }

        needs_update |= dashcdg_cdg_state_process_packet(&reader->state, &pkt);
    }

    return needs_update;
}

static int dashcdg_cdg_parity_bytes_all_zero(const struct dashcdg_subchannel_packet *pkt) {
    if (pkt == NULL) {
        return 0;
    }
    if (pkt->parity_q[0] != 0U || pkt->parity_q[1] != 0U) {
        return 0;
    }
    if (pkt->parity_p[0] != 0U || pkt->parity_p[1] != 0U || pkt->parity_p[2] != 0U || pkt->parity_p[3] != 0U) {
        return 0;
    }
    return 1;
}

static int dashcdg_cdg_known_graphics_instruction(uint8_t instruction) {
    switch (instruction & 0x3FU) {
        case DASHCDG_INSN_MEMORY_PRESET:
        case DASHCDG_INSN_BORDER_PRESET:
        case DASHCDG_INSN_TILE_BLOCK:
        case DASHCDG_INSN_SCROLL_PRESET:
        case DASHCDG_INSN_SCROLL_COPY:
        case DASHCDG_INSN_DEF_TRANSPARENT:
        case DASHCDG_INSN_LOAD_COLOR_TABLE_00:
        case DASHCDG_INSN_LOAD_COLOR_TABLE_08:
        case DASHCDG_INSN_TILE_BLOCK_XOR:
            return 1;
        default:
            return 0;
    }
}

static int dashcdg_cdg_packet_fields_plausible(const struct dashcdg_subchannel_packet *pkt) {
    uint8_t insn = pkt->instruction & 0x3FU;
    const uint8_t *d = pkt->data;

    switch (insn) {
        case DASHCDG_INSN_TILE_BLOCK:
        case DASHCDG_INSN_TILE_BLOCK_XOR: {
            uint8_t row = d[2] & 0x1FU;
            uint8_t col = d[3] & 0x3FU;

            if (row > 17U) {
                return 0;
            }
            if (col > 49U) {
                return 0;
            }
            return 1;
        }

        case DASHCDG_INSN_SCROLL_PRESET:
        case DASHCDG_INSN_SCROLL_COPY: {
            uint8_t h = d[1] & 0x3FU;
            uint8_t v = d[2] & 0x3FU;
            int h_cmd = (int) ((h & 0x30U) >> 4U);
            int v_cmd = (int) ((v & 0x30U) >> 4U);

            if (h_cmd > 2 || v_cmd > 2) {
                return 0;
            }
            return 1;
        }

        default:
            return 1;
    }
}

static int dashcdg_cdg_packet_counts_for_alignment(const uint8_t *data, size_t len, size_t offset, size_t n_scan,
                                                   size_t *out_good_header, size_t *out_good_fields, size_t *out_parity_zero) {
    size_t good_h = 0;
    size_t good_f = 0;
    size_t pz = 0;
    size_t i;

    if (data == NULL || len < offset + 24U || out_good_header == NULL || out_good_fields == NULL ||
            out_parity_zero == NULL) {
        return 0;
    }

    for (i = 0; i < n_scan; ++i) {
        const struct dashcdg_subchannel_packet *pkt =
                (const struct dashcdg_subchannel_packet *) (data + offset + i * sizeof(struct dashcdg_subchannel_packet));

        if (offset + (i + 1U) * sizeof(struct dashcdg_subchannel_packet) > len) {
            break;
        }

        if ((pkt->command & 0x3FU) != 0x09U) {
            continue;
        }
        if (!dashcdg_cdg_known_graphics_instruction(pkt->instruction)) {
            continue;
        }

        /*
         * Rips usually zero the six parity bytes after drive correction. When any
         * parity byte is present, require a valid R–W PACK RS codeword so misaligned
         * windows that accidentally match 0x09 + instruction are not over-scored.
         */
        if (!dashcdg_cdg_parity_bytes_all_zero(pkt) && !dashcdg_cdg_subchannel_pack_rs_syndrome_ok(pkt)) {
            continue;
        }

        good_h++;
        if (dashcdg_cdg_parity_bytes_all_zero(pkt)) {
            pz++;
        }
        if (dashcdg_cdg_packet_fields_plausible(pkt)) {
            good_f++;
        }
    }

    *out_good_header = good_h;
    *out_good_fields = good_f;
    *out_parity_zero = pz;
    return 1;
}

void dashcdg_cdg_compute_subchannel_trims(const uint8_t *data, size_t scan_bytes, size_t total_bytes,
                                          size_t *out_trim_prefix, size_t *out_trim_suffix) {
    size_t scan_len;
    size_t n_scan;
    size_t good_f[24];
    size_t good_h[24];
    size_t parity_z[24];
    size_t best_o;
    size_t second_f;
    size_t o;
    size_t min_good;
    size_t win_margin;
    size_t gf0;

    if (out_trim_prefix != NULL) {
        *out_trim_prefix = 0U;
    }
    if (out_trim_suffix != NULL) {
        *out_trim_suffix = 0U;
    }
    if (data == NULL || out_trim_prefix == NULL || out_trim_suffix == NULL) {
        return;
    }

    if (total_bytes < sizeof(struct dashcdg_subchannel_packet)) {
        *out_trim_suffix = total_bytes % sizeof(struct dashcdg_subchannel_packet);
        return;
    }

    scan_len = scan_bytes;
    if (scan_len > total_bytes) {
        scan_len = total_bytes;
    }

    /*
     * Use the same packet count for every trial offset so high offsets are not
     * penalized by a shorter remainder at the end of scan_len.
     */
    if (scan_len < 23U + 8U * sizeof(struct dashcdg_subchannel_packet)) {
        *out_trim_suffix = total_bytes % sizeof(struct dashcdg_subchannel_packet);
        return;
    }
    n_scan = (scan_len - 23U) / sizeof(struct dashcdg_subchannel_packet);
    if (n_scan > 1200U) {
        n_scan = 1200U;
    }
    if (n_scan < 8U) {
        *out_trim_suffix = total_bytes % sizeof(struct dashcdg_subchannel_packet);
        return;
    }

    min_good = 64U;
    if (n_scan < 128U) {
        min_good = n_scan * 2U / 5U;
        if (min_good < 12U) {
            min_good = 12U;
        }
    }

    win_margin = n_scan / 25U;
    if (win_margin < 16U) {
        win_margin = 16U;
    }

    for (o = 0; o < 24U; ++o) {
        size_t gh;
        size_t gf;
        size_t pz;

        if (scan_len < o + sizeof(struct dashcdg_subchannel_packet)) {
            good_f[o] = 0U;
            good_h[o] = 0U;
            parity_z[o] = 0U;
            continue;
        }

        if (!dashcdg_cdg_packet_counts_for_alignment(data, scan_len, o, n_scan, &gh, &gf, &pz)) {
            good_f[o] = 0U;
            good_h[o] = 0U;
            parity_z[o] = 0U;
            continue;
        }

        good_f[o] = gf;
        good_h[o] = gh;
        parity_z[o] = pz;
    }

    gf0 = good_f[0U];

    best_o = 0U;
    for (o = 1U; o < 24U; ++o) {
        if (good_f[o] > good_f[best_o]) {
            best_o = o;
        } else if (good_f[o] == good_f[best_o]) {
            if (good_h[o] > good_h[best_o]) {
                best_o = o;
            } else if (good_h[o] == good_h[best_o] && parity_z[o] > parity_z[best_o]) {
                best_o = o;
            } else if (good_h[o] == good_h[best_o] && parity_z[o] == parity_z[best_o] && o < best_o) {
                best_o = o;
            }
        }
    }

    second_f = 0U;
    for (o = 0U; o < 24U; ++o) {
        if (o == best_o) {
            continue;
        }
        if (good_f[o] > second_f) {
            second_f = good_f[o];
        }
    }

    if (best_o != 0U && good_f[best_o] >= min_good && good_f[best_o] >= second_f + win_margin && good_f[best_o] > gf0 &&
            good_f[best_o] >= gf0 + min_good / 4U) {
        *out_trim_prefix = best_o;
    } else {
        *out_trim_prefix = 0U;
    }

    if (*out_trim_prefix > total_bytes) {
        *out_trim_prefix = 0U;
    }
    {
        size_t body = total_bytes - *out_trim_prefix;

        *out_trim_suffix = body % sizeof(struct dashcdg_subchannel_packet);
    }
}
