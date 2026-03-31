#include "bhevc.h"

void bs_init_write(Bitstream *bs, uint8_t *buf, int cap) {
    bs->buf = buf;
    bs->capacity = cap;
    bs->byte_pos = 0;
    bs->bit_pos = 7;
    memset(buf, 0, cap);
}

void bs_init_read(Bitstream *bs, const uint8_t *buf, int len) {
    bs->buf = (uint8_t *)buf;
    bs->capacity = len;
    bs->byte_pos = 0;
    bs->bit_pos = 7;
}

void bs_write_bits(Bitstream *bs, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (bs->byte_pos >= bs->capacity) return;
        if ((val >> i) & 1)
            bs->buf[bs->byte_pos] |= (1 << bs->bit_pos);
        bs->bit_pos--;
        if (bs->bit_pos < 0) {
            bs->bit_pos = 7;
            bs->byte_pos++;
        }
    }
}

void bs_write_ue(Bitstream *bs, uint32_t v) {
    uint32_t vp1 = v + 1;
    int bits = 0;
    uint32_t tmp = vp1;
    while (tmp > 0) { bits++; tmp >>= 1; }
    for (int i = 0; i < bits - 1; i++)
        bs_write_bits(bs, 0, 1);
    bs_write_bits(bs, vp1, bits);
}

void bs_write_se(Bitstream *bs, int32_t v) {
    uint32_t mapped;
    if (v > 0)
        mapped = 2 * v - 1;
    else
        mapped = -2 * v;
    bs_write_ue(bs, mapped);
}

uint32_t bs_read_bits(Bitstream *bs, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        if (bs->byte_pos >= bs->capacity) return val;
        val <<= 1;
        if (bs->buf[bs->byte_pos] & (1 << bs->bit_pos))
            val |= 1;
        bs->bit_pos--;
        if (bs->bit_pos < 0) {
            bs->bit_pos = 7;
            bs->byte_pos++;
        }
    }
    return val;
}

uint32_t bs_read_ue(Bitstream *bs) {
    int leading_zeros = 0;
    while (bs_read_bits(bs, 1) == 0 && leading_zeros < 31)
        leading_zeros++;
    uint32_t val = (1 << leading_zeros) - 1;
    if (leading_zeros > 0)
        val += bs_read_bits(bs, leading_zeros);
    return val;
}

int32_t bs_read_se(Bitstream *bs) {
    uint32_t mapped = bs_read_ue(bs);
    if (mapped & 1)
        return (int32_t)((mapped + 1) >> 1);
    else
        return -(int32_t)(mapped >> 1);
}

void bs_align_byte(Bitstream *bs) {
    if (bs->bit_pos != 7) {
        bs->byte_pos++;
        bs->bit_pos = 7;
    }
}

int bs_pos_bytes(const Bitstream *bs) {
    return bs->byte_pos + (bs->bit_pos < 7 ? 1 : 0);
}
