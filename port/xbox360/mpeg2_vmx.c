#include "config.h"
#include <stdint.h>
#include <string.h>
#include <altivec.h>
#include "../../deps/libmpeg2/include/mpeg2.h"
#include "../../deps/libmpeg2/libmpeg2/mpeg2_internal.h"

extern mpeg2_mc_t mpeg2_mc;
static unsigned vmx_blocks;
static unsigned vmx_dc_blocks;
extern void (*mpeg2_idct_add)(int, int16_t *, uint8_t *, int);
static void (*scalar_idct_add)(int, int16_t *, uint8_t *, int);
typedef __vector unsigned char bytes16;
typedef __vector unsigned short words8;
typedef __vector signed short shorts8;
typedef __vector signed int ints4;
typedef __vector unsigned int uints4;

extern void (*dvd360_idct_columns_vmx)(int16_t *);
void dvd360_idct_columns_scalar(int16_t *);
static unsigned idct_blocks;

unsigned dvd360_mpeg2_vmx_idct_blocks(void) { return idct_blocks; }

static inline __attribute__((always_inline)) ints4 idct_mul(ints4 value, unsigned short coefficient)
{
    // Xenon lacks the needed integer vector multiply. All coefficients
    // are compile-time constants; unrolling leaves only their set bits.
    uints4 product = vec_splats(0u);
#pragma clang loop unroll(full)
    for (unsigned bit = 0; bit < 12; ++bit)
        if (coefficient & (1u << bit))
            product += vec_sl((uints4)value, vec_splats(bit));
    return (ints4)product;
}

static inline __attribute__((always_inline)) void butterfly(ints4 a, ints4 b, unsigned short w0,
                             unsigned short w1, ints4 *p, ints4 *q)
{
    *p = idct_mul(a, w0) + idct_mul(b, w1);
    *q = idct_mul(b, w0) - idct_mul(a, w1);
}

static inline ints4 asr(ints4 v, unsigned n)
{
    return vec_sra(v, vec_splats(n));
}

static void idct_half(const ints4 d[8], ints4 out[8])
{
    const ints4 d0 = vec_sl(d[0], vec_splats(11u)) + vec_splats(65536);
    const ints4 d2 = vec_sl(d[2], vec_splats(11u));
    ints4 t2, t3;
    butterfly(d[3], d[1], 1108, 2676, &t2, &t3);
    const ints4 a0 = d0 + d2 + t2, a1 = d0 - d2 + t3;
    const ints4 a2 = d0 - d2 - t3, a3 = d0 + d2 - t2;
    ints4 t0, t1;
    butterfly(d[7], d[4], 565, 2841, &t0, &t1);
    butterfly(d[5], d[6], 2408, 1609, &t2, &t3);
    const ints4 b0 = t0 + t2, b3 = t1 + t3;
    const ints4 b1 = idct_mul(asr(t0 - t2 + t1 - t3, 8), 181);
    const ints4 b2 = idct_mul(asr(t0 - t2 - t1 + t3, 8), 181);
    out[0] = asr(a0 + b0, 17); out[7] = asr(a0 - b0, 17);
    out[1] = asr(a1 + b1, 17); out[6] = asr(a1 - b1, 17);
    out[2] = asr(a2 + b2, 17); out[5] = asr(a2 - b2, 17);
    out[3] = asr(a3 + b3, 17); out[4] = asr(a3 - b3, 17);
}

static void idct_columns(int16_t *block)
{
    ints4 hi[8], lo[8], out_hi[8], out_lo[8];
    for (unsigned i = 0; i < 8; ++i) {
        const shorts8 row = vec_ld(0, block + i * 8);
        hi[i] = vec_unpackh(row);
        lo[i] = vec_unpackl(row);
    }
    idct_half(hi, out_hi);
    idct_half(lo, out_lo);
    for (unsigned i = 0; i < 8; ++i)
        vec_st(vec_pack(out_hi[i], out_lo[i]), 0, block + i * 8);
    ++idct_blocks;
}

int dvd360_mpeg2_enable_vmx_idct(void)
{
    _Alignas(16) int16_t expected[64], actual[64];
    uint32_t random = 1;
    for (unsigned test = 0; test < 256; ++test) {
        for (unsigned i = 0; i < 64; ++i) {
            random = random * 1664525u + 1013904223u;
            expected[i] = test == 0 ? 0 : test == 1 ? INT16_MIN :
                          test == 2 ? INT16_MAX : (int16_t)(random >> 16);
        }
        memcpy(actual, expected, sizeof(actual));
        dvd360_idct_columns_scalar(expected);
        idct_columns(actual);
        if (memcmp(actual, expected, sizeof(actual))) {
            idct_blocks = 0;
            return 0;
        }
    }
    idct_blocks = 0;
    dvd360_idct_columns_vmx = idct_columns;
    return 1;
}

unsigned dvd360_mpeg2_vmx_blocks(void)
{
    return vmx_blocks;
}

unsigned dvd360_mpeg2_vmx_dc_blocks(void)
{
    return vmx_dc_blocks;
}

static void add_dc(int last, int16_t *block, uint8_t *dst, int stride)
{
    if (last != 129 || (block[0] & (7 << 4)) == (4 << 4)) {
        scalar_idct_add(last, block, dst, stride);
        return;
    }

    const __vector signed short dc = vec_splats((short)((block[0] + 64) >> 7));
    const bytes16 zero = vec_splat_u8(0);
    block[0] = block[63] = 0;
    ++vmx_dc_blocks;
    for (unsigned row = 0; row < 8; ++row, dst += stride) {
        // Only eight bytes belong to this block. Do not read or write its neighbor.
        bytes16 pixels = zero;
        memcpy(&pixels, dst, 8);
        const __vector signed short sum = vec_add(
            (__vector signed short)vec_mergeh(zero, pixels), dc);
        pixels = vec_packsu(sum, sum);
        memcpy(dst, &pixels, 8);
    }
}

int dvd360_mpeg2_enable_vmx_dc(void)
{
    _Alignas(16) int16_t expected_block[64], actual_block[64];
    _Alignas(16) uint8_t expected[144], actual[144];
    static const int16_t coefficients[] = {-16384, -192, -64, 0, 63, 64, 192, 16352};
    scalar_idct_add = mpeg2_idct_add;
    for (unsigned test = 0; test < sizeof(coefficients) / sizeof(coefficients[0]); ++test) {
        memset(expected_block, 0, sizeof(expected_block));
        expected_block[0] = coefficients[test];
        expected_block[63] = 1;
        memcpy(actual_block, expected_block, sizeof(actual_block));
        for (unsigned i = 0; i < sizeof(actual); ++i)
            actual[i] = expected[i] = (uint8_t)(i * 53);
        scalar_idct_add(129, expected_block, expected + 1, 17);
        add_dc(129, actual_block, actual + 1, 17);
        if (memcmp(expected, actual, sizeof(actual)) ||
            memcmp(expected_block, actual_block, sizeof(actual_block))) {
            vmx_dc_blocks = 0;
            return 0;
        }
    }
    vmx_dc_blocks = 0;
    mpeg2_idct_add = add_dc;
    return 1;
}

static inline bytes16 read16(const uint8_t *src)
{
    if (!((uintptr_t)src & 15))
        return vec_ld(0, src);

    // RXDK's assembler doesn't recognize these Xenon mnemonics.
    // lvlx128 and lvrx128 zero the unused lanes; OR joins the two halves.
    bytes16 left, right;
    __asm__ volatile(".long (0x10000403 | (%0 << 21) | (%1 << 11))"
                     : "=v"(left) : "b"(src) : "memory");
    __asm__ volatile(".long (0x10000443 | (%0 << 21) | (%1 << 11))"
                     : "=v"(right) : "b"(src + 16) : "memory");
    return vec_or(left, right);
}

static inline bytes16 average4(bytes16 a, bytes16 b, bytes16 c, bytes16 d)
{
    const bytes16 zero = vec_splat_u8(0);
    const words8 two = vec_splat_u16(2);
    // Widen before summing; chained rounded byte averages give different pixels.
#define SUM4(merge) vec_sr(vec_add(vec_add( \
        vec_add((words8)merge(zero, a), (words8)merge(zero, b)), \
        vec_add((words8)merge(zero, c), (words8)merge(zero, d))), two), two)
    const words8 hi = SUM4(vec_mergeh), lo = SUM4(vec_mergel);
#undef SUM4
    return vec_pack(hi, lo);
}

#define MC16(name, mode, blend) \
static void name(uint8_t *dst, const uint8_t *src, int stride, int height) \
{ \
    if (((uintptr_t)dst | (unsigned)stride) & 15) { \
        (blend ? mpeg2_mc_c.avg[mode] : mpeg2_mc_c.put[mode])(dst, src, stride, height); \
        return; \
    } \
    ++vmx_blocks; \
    for (; height > 0; --height, dst += stride, src += stride) { \
        bytes16 p = read16(src); \
        if (mode == 1) p = vec_avg(p, read16(src + 1)); \
        if (mode == 2) p = vec_avg(p, read16(src + stride)); \
        if (mode == 3) p = average4(p, read16(src + 1), read16(src + stride), \
                                    read16(src + stride + 1)); \
        if (blend) p = vec_avg(p, vec_ld(0, dst)); \
        vec_st(p, 0, dst); \
    } \
}

MC16(put_o, 0, 0)
MC16(put_x, 1, 0)
MC16(put_y, 2, 0)
MC16(put_xy, 3, 0)
MC16(avg_o, 0, 1)
MC16(avg_x, 1, 1)
MC16(avg_y, 2, 1)
MC16(avg_xy, 3, 1)

int dvd360_mpeg2_enable_vmx(void)
{
    _Alignas(16) uint8_t src[320], expected[320], actual[320];
    mpeg2_mc_fct *const kernels[2][4] = {
        {put_o, put_x, put_y, put_xy}, {avg_o, avg_x, avg_y, avg_xy}
    };
    for (unsigned blend = 0; blend < 2; ++blend) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            for (unsigned offset = 0; offset < 16; ++offset) {
                for (unsigned i = 0; i < sizeof(src); ++i) {
                    src[i] = (uint8_t)(i * 37 + (i >> 2) + 17);
                    actual[i] = expected[i] = (uint8_t)(i * 13 + 4);
                }
                (blend ? mpeg2_mc_c.avg[mode] : mpeg2_mc_c.put[mode])
                    (expected, src + offset, 32, 8);
                kernels[blend][mode](actual, src + offset, 32, 8);
                if (memcmp(expected, actual, sizeof(actual))) {
                    vmx_blocks = 0;
                    return 0;
                }
            }
        }
    }
    vmx_blocks = 0;
    for (unsigned mode = 0; mode < 4; ++mode) {
        mpeg2_mc.put[mode] = kernels[0][mode];
        mpeg2_mc.avg[mode] = kernels[1][mode];
    }
    return 1;
}
