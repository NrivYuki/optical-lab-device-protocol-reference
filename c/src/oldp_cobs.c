#include "oldp_cobs.h"

size_t oldp_cobs_encode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t o = 1, code_at = 0, i;
    uint8_t code = 1;
    for (i = 0; i < n; i++) {
        if (in[i] == 0) {
            out[code_at] = code; code_at = o++; code = 1;
            continue;
        }
        out[o++] = in[i]; code++;
        /* 0 の無い 254 バイトで区切る。続きがあるときだけ、次の区切りを始める */
        if (code == 0xff && i + 1 < n) { out[code_at] = code; code_at = o++; code = 1; }
    }
    out[code_at] = code;
    return o;
}

size_t oldp_cobs_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t i = 0, o = 0, j;
    while (i < n) {
        uint8_t code = in[i];
        if (code == 0 || i + code > n) return (size_t)-1;
        for (j = 1; j < code; j++) {
            if (in[i + j] == 0 || o >= cap) return (size_t)-1;
            out[o++] = in[i + j];
        }
        i += code;
        if (code < 0xff && i < n) { if (o >= cap) return (size_t)-1; out[o++] = 0; }
    }
    return o;
}

uint32_t oldp_crc32(const uint8_t *data, size_t n) {
    uint32_t c = 0xffffffffUL;
    uint8_t k;
    while (n--) {
        c ^= *data++;
        for (k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320UL ^ (c >> 1)) : (c >> 1);
    }
    return c ^ 0xffffffffUL;
}
