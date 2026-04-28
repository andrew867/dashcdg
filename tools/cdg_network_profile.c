/*
 * Offline CD+G hazard profile for v4 / Wi-Fi receivers.
 * Same trim + packed layout assumptions as desktop TX (`dashcdg_cdg_compute_subchannel_trims`).
 *
 * Usage: cdg-network-profile path/to/track.cdg
 */

#include "dashcdg/cdg.h"
#include "dashcdg/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void apply_subchannel_trims(uint8_t *bytes, size_t *inout_size, size_t trim_prefix, size_t trim_suffix) {
    size_t new_size;

    if (bytes == NULL || inout_size == NULL) {
        return;
    }
    if (trim_prefix == 0U && trim_suffix == 0U) {
        return;
    }
    if (trim_prefix + trim_suffix > *inout_size) {
        return;
    }
    new_size = *inout_size - trim_prefix - trim_suffix;
    if (trim_prefix > 0U && new_size > 0U) {
        memmove(bytes, bytes + trim_prefix, new_size);
    }
    *inout_size = new_size;
}

static uint8_t *read_entire_file(const char *path, size_t *out_size) {
    FILE *f;
    uint8_t *buf;
    long sz;

    if (path == NULL || out_size == NULL) {
        return NULL;
    }
    *out_size = 0U;
    f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (uint8_t *) malloc((size_t) sz);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t) sz, f) != (size_t) sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t) sz;
    return buf;
}

int main(int argc, char **argv) {
    const char *path;
    uint8_t *raw = NULL;
    size_t raw_size = 0U;
    size_t trim_prefix = 0U;
    size_t trim_suffix = 0U;
    struct dashcdg_cdg_reader reader;
    struct dashcdg_subchannel_packet pkt;
    uint64_t leading_non_graphics_ticks = 0U;
    int saw_first_graphics = 0;
    uint64_t tile_copy = 0U;
    uint64_t tile_xor = 0U;
    uint64_t memory_preset_total = 0U;
    uint64_t memory_preset_clear = 0U;
    uint64_t scroll = 0U;
    uint64_t color_table = 0U;
    uint64_t border = 0U;
    uint64_t def_transparent = 0U;
    uint64_t unknown_graphics = 0U;
    dashcdg_tick_t max_key_gap_ticks = 0U;
    size_t k = 0U;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.cdg>\n", argv[0]);
        return 2;
    }
    path = argv[1];
    raw = read_entire_file(path, &raw_size);
    if (raw == NULL) {
        return 1;
    }

    dashcdg_cdg_compute_subchannel_trims(raw, raw_size, raw_size, &trim_prefix, &trim_suffix);
    apply_subchannel_trims(raw, &raw_size, trim_prefix, trim_suffix);

    dashcdg_cdg_reader_init(&reader);
    if (!dashcdg_cdg_reader_load_memory(&reader, raw, raw_size)) {
        free(raw);
        fprintf(stderr, "cdg: load_memory failed\n");
        return 1;
    }
    free(raw);
    raw = NULL;

    if (!dashcdg_cdg_reader_build_keyframes(&reader)) {
        dashcdg_cdg_reader_free(&reader);
        fprintf(stderr, "cdg: build_keyframes failed\n");
        return 1;
    }

    for (k = 1U; k < reader.keyframes.count; ++k) {
        dashcdg_tick_t gap = reader.keyframes.items[k].timestamp - reader.keyframes.items[k - 1U].timestamp;

        if (gap > max_key_gap_ticks) {
            max_key_gap_ticks = gap;
        }
    }

    dashcdg_cdg_reader_reset(&reader);
    while (dashcdg_cdg_reader_read_packet(&reader, &pkt)) {
        if ((pkt.command & 0x3FU) != 0x09U) {
            reader.state.ts++;
            if (!saw_first_graphics) {
                leading_non_graphics_ticks++;
            }
            continue;
        }

        saw_first_graphics = 1;
        {
            uint8_t insn = pkt.instruction & 0x3FU;

            switch (insn) {
                case DASHCDG_INSN_TILE_BLOCK:
                    tile_copy++;
                    break;
                case DASHCDG_INSN_TILE_BLOCK_XOR:
                    tile_xor++;
                    break;
                case DASHCDG_INSN_MEMORY_PRESET: {
                    const struct dashcdg_insn_memory_preset *pr = (const struct dashcdg_insn_memory_preset *) pkt.data;

                    memory_preset_total++;
                    if ((pr->repeat & 0x0FU) == 0U) {
                        memory_preset_clear++;
                    }
                    break;
                }
                case DASHCDG_INSN_SCROLL_PRESET:
                case DASHCDG_INSN_SCROLL_COPY:
                    scroll++;
                    break;
                case DASHCDG_INSN_LOAD_COLOR_TABLE_00:
                case DASHCDG_INSN_LOAD_COLOR_TABLE_08:
                    color_table++;
                    break;
                case DASHCDG_INSN_BORDER_PRESET:
                    border++;
                    break;
                case DASHCDG_INSN_DEF_TRANSPARENT:
                    def_transparent++;
                    break;
                default:
                    unknown_graphics++;
                    break;
            }
        }
        (void) dashcdg_cdg_state_process_packet(&reader.state, &pkt);
    }

    {
        uint64_t total_tiles = tile_copy + tile_xor;
        uint64_t xor_pct_x100 = total_tiles > 0U ? (tile_xor * 10000ULL) / total_tiles : 0U;
        uint64_t duration_ms = dashcdg_packet_count_to_ms(reader.state.ts);
        uint64_t clears_per_min_x100 =
                duration_ms > 0U ? (memory_preset_clear * 6000000ULL) / duration_ms : 0U;

        printf("file: %s\n", path);
        printf("trim_prefix_bytes: %zu trim_suffix_bytes: %zu packed_packets: %zu file_bytes: %zu\n",
               trim_prefix,
               trim_suffix,
               raw_size / sizeof(struct dashcdg_subchannel_packet),
               raw_size);
        printf("duration_est_packets: %llu (~%llu ms)\n",
               (unsigned long long) reader.state.ts,
               (unsigned long long) duration_ms);
        printf("leading_non_graphics_ticks_before_first_0x09: %llu\n", (unsigned long long) leading_non_graphics_ticks);
        printf("keyframes_memory_clear_repeat0: %zu\n", reader.keyframes.count);
        printf("max_ticks_between_keyframes: %llu (~%llu ms)\n",
               (unsigned long long) max_key_gap_ticks,
               (unsigned long long) dashcdg_packet_count_to_ms(max_key_gap_ticks));
        printf("memory_preset_packets: %llu (full_screen_clears_repeat0: %llu)\n",
               (unsigned long long) memory_preset_total,
               (unsigned long long) memory_preset_clear);
        printf("tile_block_copy: %llu tile_block_xor: %llu xor_pct: %llu.%02llu\n",
               (unsigned long long) tile_copy,
               (unsigned long long) tile_xor,
               (unsigned long long) (xor_pct_x100 / 100ULL),
               (unsigned long long) (xor_pct_x100 % 100ULL));
        printf("scroll_ops: %llu color_table_ops: %llu border: %llu def_transparent: %llu unknown_graphics: %llu\n",
               (unsigned long long) scroll,
               (unsigned long long) color_table,
               (unsigned long long) border,
               (unsigned long long) def_transparent,
               (unsigned long long) unknown_graphics);
        printf("full_screen_clears_per_minute_est: %llu.%02llu\n",
               (unsigned long long) (clears_per_min_x100 / 100ULL),
               (unsigned long long) (clears_per_min_x100 % 100ULL));
        printf(
                "\nInterpretation (v4 + Wi-Fi):\n"
                "- Large max_ticks_between_keyframes + high xor_pct => one lost delta corrupts the canvas for a long wall-clock span until the next anchor / MEMORY clear.\n"
                "- Raise TX CDG FEC / shorten anchor interval / improve Wi-Fi PHY if numbers look hostile.\n");
    }

    dashcdg_cdg_reader_free(&reader);
    return 0;
}
