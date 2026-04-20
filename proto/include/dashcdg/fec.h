#ifndef DASHCDG_FEC_H
#define DASHCDG_FEC_H

#include <stddef.h>
#include <stdint.h>

#include "dashcdg/protocol.h"

struct dashcdg_fec_parity_state {
    uint16_t payload_bytes;
    uint16_t payload_length_xor;
    uint8_t payload_xor[DASHCDG_MAX_FEC_PAYLOAD_BYTES];
};

void dashcdg_fec_parity_init(struct dashcdg_fec_parity_state *state);
int dashcdg_fec_parity_accumulate(
        struct dashcdg_fec_parity_state *state,
        const uint8_t *payload,
        uint16_t payload_length
);
int dashcdg_fec_parity_recover(
        const struct dashcdg_fec_parity_state *parity,
        const uint8_t *const known_payloads[],
        const uint16_t known_lengths[],
        size_t known_count,
        uint8_t *recovered_payload,
        uint16_t *recovered_length
);

#endif
