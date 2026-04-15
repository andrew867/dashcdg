#include "dashcdg/cdg.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint16_t dashcdg_be16_to_host(uint16_t value) {
    return (uint16_t) (((value & 0x00FFU) << 8U) | ((value & 0xFF00U) >> 8U));
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

            return 1;
        }

        case DASHCDG_INSN_MEMORY_PRESET: {
            const struct dashcdg_insn_memory_preset *preset = (const struct dashcdg_insn_memory_preset *) data;

            if ((preset->repeat & 0x0FU) == 0U) {
                memset(state->framebuffer, preset->color & 0x0FU, sizeof(state->framebuffer));
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

            return 1;
        }

        case DASHCDG_INSN_DEF_TRANSPARENT: {
            const struct dashcdg_insn_define_transparent *transparent =
                    (const struct dashcdg_insn_define_transparent *) data;

            for (size_t i = 0; i < DASHCDG_COLORS; ++i) {
                state->transparency[i] = transparent->transparent[i] & 0x3FU;
            }

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
