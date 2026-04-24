/*
 * MIT License
 *
 * Copyright (C) 2026 Andrew Green
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * ---
 * GF(2^6) Reed–Solomon parity for CD R–W subchannel PACKs (24 bytes, 6-bit symbols),
 * per IEC 60908 / Red Book PACK layout (18 payload symbols + 2 Q + 4 P over GF(64),
 * primitive polynomial x^6 + x + 1). Tests in tests/test_core.c pin golden packet
 * bytes and round-trip behavior for this encoder and syndrome formulation.
 */

#include "dashcdg/cdg.h"

#include <string.h>

enum {
    DASHCDG_RS_SUB_RW_BITS = 6,
    DASHCDG_RS_SUB_RW_MOD = (1U << DASHCDG_RS_SUB_RW_BITS) - 1U,
    DASHCDG_LSUB_RAW = 18,
    DASHCDG_LSUB_QRAW = 2,
    DASHCDG_LSUB_Q = 2,
    DASHCDG_LSUB_P = 4
};

static const uint8_t g_rs_sub_rw_alog[64] = {
    1,  2,  4,  8,  16, 32, 3,  6,  12, 24, 48, 35, 5,  10, 20, 40, 19, 38, 15, 30, 60, 59, 53, 41, 17, 34, 7,  14, 28, 56, 51, 37,
    9,  18, 36, 11, 22, 44, 27, 54, 47, 29, 58, 55, 45, 25, 50, 39, 13, 26, 52, 43, 21, 42, 23, 46, 31, 62, 63, 61, 57, 49, 33, 0,
};

static const uint8_t g_rs_sub_rw_log[64] = {
    0,  0,  1,  6,  2,  12, 7,  26, 3,  32, 13, 35, 8,  48, 27, 18, 4,  24, 33, 16, 14, 52, 36, 54, 9,  45, 49, 38, 28, 41, 19, 56,
    5,  62, 25, 11, 34, 31, 17, 47, 15, 23, 53, 51, 37, 44, 55, 40, 10, 61, 46, 30, 50, 22, 39, 43, 29, 60, 42, 21, 20, 59, 57, 58,
};

static const uint8_t g_sq[2][2] = {
    {26, 6},
    {7,  1},
};

static const uint8_t g_sp[4][20] = {
    {57, 38, 44, 29, 17, 57, 53, 58, 60, 39, 12, 38, 18, 41, 6,  25, 39, 37, 5,  18},
    {38, 62, 42, 13, 30, 11, 46, 5,  54, 26, 12, 49, 48, 46, 8,  50, 28, 9,  12, 39},
    {32, 18, 41, 49, 52, 62, 38, 36, 39, 58, 37, 24, 34, 51, 51, 27, 28, 36, 22, 21},
    {44, 50, 35, 23, 0,  59, 1,  3,  45, 18, 44, 24, 47, 12, 31, 45, 43, 11, 24, 6},
};

static void dashcdg_rs_sub_encode_q(uint8_t *io24) {
    uint8_t *q;
    int i;

    memmove(io24 + DASHCDG_LSUB_QRAW + DASHCDG_LSUB_Q, io24 + DASHCDG_LSUB_QRAW, (size_t) DASHCDG_LSUB_RAW - (size_t) DASHCDG_LSUB_QRAW);
    q = io24 + DASHCDG_LSUB_QRAW;
    memset(q, 0, (size_t) DASHCDG_LSUB_Q);

    for (i = 0; i < DASHCDG_LSUB_QRAW; ++i) {
        uint8_t data = (uint8_t) (io24[i] & 0x3FU);

        if (data != 0U) {
            uint8_t base = g_rs_sub_rw_log[data];

            q[0] = (uint8_t) (q[0] ^ g_rs_sub_rw_alog[(size_t) (base + g_sq[0][i]) % DASHCDG_RS_SUB_RW_MOD]);
            q[1] = (uint8_t) (q[1] ^ g_rs_sub_rw_alog[(size_t) (base + g_sq[1][i]) % DASHCDG_RS_SUB_RW_MOD]);
        }
    }
}

static void dashcdg_rs_sub_encode_p(uint8_t *io24) {
    uint8_t *p;
    int i;

    p = io24 + DASHCDG_LSUB_RAW + DASHCDG_LSUB_Q;
    memset(p, 0, (size_t) DASHCDG_LSUB_P);

    for (i = 0; i < DASHCDG_LSUB_RAW + DASHCDG_LSUB_Q; ++i) {
        uint8_t data = (uint8_t) (io24[i] & 0x3FU);

        if (data != 0U) {
            uint8_t base = g_rs_sub_rw_log[data];

            p[0] = (uint8_t) (p[0] ^ g_rs_sub_rw_alog[(size_t) (base + g_sp[0][i]) % DASHCDG_RS_SUB_RW_MOD]);
            p[1] = (uint8_t) (p[1] ^ g_rs_sub_rw_alog[(size_t) (base + g_sp[1][i]) % DASHCDG_RS_SUB_RW_MOD]);
            p[2] = (uint8_t) (p[2] ^ g_rs_sub_rw_alog[(size_t) (base + g_sp[2][i]) % DASHCDG_RS_SUB_RW_MOD]);
            p[3] = (uint8_t) (p[3] ^ g_rs_sub_rw_alog[(size_t) (base + g_sp[3][i]) % DASHCDG_RS_SUB_RW_MOD]);
        }
    }
}

static int dashcdg_rs_sub_syndrome_q_nonzero(const uint8_t *p24) {
    uint8_t q[DASHCDG_LSUB_Q];
    int i;

    memset(q, 0, sizeof(q));
    for (i = DASHCDG_LSUB_QRAW + DASHCDG_LSUB_Q - 1; i >= 0; --i) {
        uint8_t data = (uint8_t) (p24[DASHCDG_LSUB_QRAW + DASHCDG_LSUB_Q - 1 - i] & 0x3FU);

        if (data != 0U) {
            uint8_t base = g_rs_sub_rw_log[data];

            q[0] = (uint8_t) (q[0] ^ g_rs_sub_rw_alog[(size_t) (base + (unsigned) i * 0U) % DASHCDG_RS_SUB_RW_MOD]);
            q[1] = (uint8_t) (q[1] ^ g_rs_sub_rw_alog[(size_t) (base + (unsigned) i * 1U) % DASHCDG_RS_SUB_RW_MOD]);
        }
    }
    return (q[0] != 0U || q[1] != 0U) ? 1 : 0;
}

static int dashcdg_rs_sub_syndrome_p_nonzero(const uint8_t *p24) {
    uint8_t p[DASHCDG_LSUB_P];
    int i;

    memset(p, 0, sizeof(p));
    for (i = DASHCDG_LSUB_RAW + DASHCDG_LSUB_Q + DASHCDG_LSUB_P - 1; i >= 0; --i) {
        uint8_t data = (uint8_t) (p24[DASHCDG_LSUB_RAW + DASHCDG_LSUB_Q + DASHCDG_LSUB_P - 1 - i] & 0x3FU);

        if (data != 0U) {
            uint8_t base = g_rs_sub_rw_log[data];

            p[0] = (uint8_t) (p[0] ^ g_rs_sub_rw_alog[(size_t) (base + (unsigned) i * 0U) % DASHCDG_RS_SUB_RW_MOD]);
            p[1] = (uint8_t) (p[1] ^ g_rs_sub_rw_alog[(size_t) (base + (unsigned) i * 1U) % DASHCDG_RS_SUB_RW_MOD]);
            p[2] = (uint8_t) (p[2] ^ g_rs_sub_rw_alog[(size_t) (base + (unsigned) i * 2U) % DASHCDG_RS_SUB_RW_MOD]);
            p[3] = (uint8_t) (p[3] ^ g_rs_sub_rw_alog[(size_t) (base + (unsigned) i * 3U) % DASHCDG_RS_SUB_RW_MOD]);
        }
    }
    return (p[0] != 0U || p[1] != 0U || p[2] != 0U || p[3] != 0U) ? 1 : 0;
}

int dashcdg_cdg_subchannel_pack_rs_syndrome_ok(const struct dashcdg_subchannel_packet *pkt) {
    uint8_t buf[sizeof(struct dashcdg_subchannel_packet)];

    if (pkt == NULL) {
        return 0;
    }
    memcpy(buf, pkt, sizeof(buf));
    if (dashcdg_rs_sub_syndrome_q_nonzero(buf) != 0) {
        return 0;
    }
    if (dashcdg_rs_sub_syndrome_p_nonzero(buf) != 0) {
        return 0;
    }
    return 1;
}

void dashcdg_cdg_subchannel_pack_rs_fill(struct dashcdg_subchannel_packet *pkt) {
    uint8_t work[sizeof(struct dashcdg_subchannel_packet)];

    if (pkt == NULL) {
        return;
    }
    memset(work, 0, sizeof(work));
    work[0] = (uint8_t) (pkt->command & 0x3FU);
    work[1] = (uint8_t) (pkt->instruction & 0x3FU);
    {
        size_t j;

        for (j = 0; j < sizeof(pkt->data); ++j) {
            work[2U + j] = (uint8_t) (pkt->data[j] & 0x3FU);
        }
    }
    dashcdg_rs_sub_encode_q(work);
    dashcdg_rs_sub_encode_p(work);
    memcpy(pkt, work, sizeof(*pkt));
}
