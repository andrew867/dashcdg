#include "dashcdg/fec.h"

#include <string.h>

void dashcdg_fec_parity_init(struct dashcdg_fec_parity_state *state) {
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
}

int dashcdg_fec_parity_accumulate(
        struct dashcdg_fec_parity_state *state,
        const uint8_t *payload,
        uint16_t payload_length
) {
    if (state == NULL || payload == NULL || payload_length == 0 || payload_length > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
        return 0;
    }

    if (payload_length > state->payload_bytes) {
        state->payload_bytes = payload_length;
    }
    state->payload_length_xor ^= payload_length;

    for (uint16_t i = 0; i < payload_length; ++i) {
        state->payload_xor[i] ^= payload[i];
    }

    return 1;
}

int dashcdg_fec_parity_recover(
        const struct dashcdg_fec_parity_state *parity,
        const uint8_t *const known_payloads[],
        const uint16_t known_lengths[],
        size_t known_count,
        uint8_t *recovered_payload,
        uint16_t *recovered_length
) {
    uint16_t length = 0;

    if (parity == NULL || known_payloads == NULL || known_lengths == NULL || recovered_payload == NULL || recovered_length == NULL) {
        return 0;
    }
    if (parity->payload_bytes == 0 || parity->payload_bytes > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
        return 0;
    }

    memcpy(recovered_payload, parity->payload_xor, parity->payload_bytes);
    length = parity->payload_length_xor;

    for (size_t i = 0; i < known_count; ++i) {
        if (known_payloads[i] == NULL || known_lengths[i] == 0 || known_lengths[i] > DASHCDG_MAX_FEC_PAYLOAD_BYTES) {
            return 0;
        }
        length ^= known_lengths[i];
        for (uint16_t j = 0; j < known_lengths[i]; ++j) {
            recovered_payload[j] ^= known_payloads[i][j];
        }
    }

    if (length == 0 || length > parity->payload_bytes) {
        return 0;
    }

    *recovered_length = length;
    return 1;
}
