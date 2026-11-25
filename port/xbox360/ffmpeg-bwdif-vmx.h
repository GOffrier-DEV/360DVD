/* RXDK implementation of FFmpeg's LGPL bwdif interior scanline arithmetic. */
#include <altivec.h>
#include <stdint.h>
#include "bwdifdsp.h"

typedef __vector unsigned char BwdifBytes;
typedef __vector signed int BwdifWords;

static inline BwdifWords bwdif_abs(BwdifWords a)
{
    return vec_max(a, -a);
}

static inline __vector float bwdif_product(BwdifWords a, float coefficient)
{
    return vec_madd(vec_ctf(a, 0), vec_splats(coefficient), vec_splats(0.0f));
}

/* Widen unsigned bytes with merges, not signed byte unpacking. */
#define BWDIF_WIDEN(a, half, quarter) \
    ((BwdifWords)vec_merge##quarter((__vector unsigned short)zero, \
        (__vector unsigned short)vec_merge##half(zero, (a))))

static void dvd360_bwdif_line(void *dst0, const void *prev0, const void *cur0,
    const void *next0, int w, int p, int m, int p2, int m2, int p3, int m3,
    int p4, int m4, int parity, int clip_max)
{
    uint8_t *dst = dst0;
    const uint8_t *prev = prev0, *cur = cur0, *next = next0;
    const uint8_t *a = parity ? prev : cur, *b = parity ? cur : next;
    const BwdifBytes zero = vec_splats((unsigned char)0);
    const BwdifWords z = vec_splats(0);
    const __vector unsigned int one = vec_splats(1u);
    int x = 0;
    /* vec_ld/st round down: use them only on aligned rows. */
    if (clip_max == 255 &&
        !(((uintptr_t)dst | (uintptr_t)prev | (uintptr_t)cur | (uintptr_t)next |
           p | m | p2 | m2 | p3 | m3 | p4 | m4) & 15)) {
        for (; x + 16 <= w; x += 16) {
            const BwdifBytes c8 = vec_ld(x + m, cur), e8 = vec_ld(x + p, cur);
            const BwdifBytes a8 = vec_ld(x, a), b8 = vec_ld(x, b);
            const BwdifBytes pm = vec_ld(x + m, prev), pp = vec_ld(x + p, prev);
            const BwdifBytes nm = vec_ld(x + m, next), np = vec_ld(x + p, next);
            if (vec_all_eq(a8, b8) && vec_all_eq(c8, pm) && vec_all_eq(e8, pp) &&
                vec_all_eq(c8, nm) && vec_all_eq(e8, np)) {
                vec_st(a8, x, dst);
                continue;
            }
            const BwdifBytes am2 = vec_ld(x + m2, a), bm2 = vec_ld(x + m2, b);
            const BwdifBytes ap2 = vec_ld(x + p2, a), bp2 = vec_ld(x + p2, b);
            const BwdifBytes am4 = vec_ld(x + m4, a), bm4 = vec_ld(x + m4, b);
            const BwdifBytes ap4 = vec_ld(x + p4, a), bp4 = vec_ld(x + p4, b);
            const BwdifBytes cm3 = vec_ld(x + m3, cur), cp3 = vec_ld(x + p3, cur);
            BwdifWords r0, r1, r2, r3;
#define BWDIF_QUARTER(result) do { \
    const BwdifWords c = V(c8), e = V(e8), sum = V(a8) + V(b8); \
    const BwdifWords d = vec_sra(sum, one), delta = bwdif_abs(V(a8) - V(b8)); \
    BwdifWords diff = vec_max(vec_sra(delta, one), vec_max( \
        vec_sra(bwdif_abs(V(pm) - c) + bwdif_abs(V(pp) - e), one), \
        vec_sra(bwdif_abs(V(nm) - c) + bwdif_abs(V(np) - e), one))); \
    const __vector bool int still = vec_cmpeq(diff, z); \
    const BwdifWords sm = V(am2) + V(bm2), sp = V(ap2) + V(bp2); \
    const BwdifWords f = vec_sra(sp, one) - e, g = vec_sra(sm, one) - c; \
    const BwdifWords dc = d - c, de = d - e; \
    diff = vec_max(diff, vec_max(vec_min(vec_min(dc, de), vec_max(f, g)), \
        -vec_max(vec_max(dc, de), vec_min(f, g)))); \
    const BwdifWords ce = c + e, c3 = V(cm3) + V(cp3); \
    /* All products and sums here fit exactly in the 24-bit float mantissa. */ \
    const BwdifWords hf = vec_cts(bwdif_product(sum, 5570.0f) \
        - bwdif_product(sm + sp, 3801.0f) \
        + bwdif_product(V(am4) + V(bm4) + V(ap4) + V(bp4), 1016.0f), 0); \
    const BwdifWords lf = vec_cts(bwdif_product(ce, 4309.0f) \
        - bwdif_product(c3, 213.0f), 0); \
    const BwdifWords spatial = vec_cts(bwdif_product(ce, 5077.0f) \
        - bwdif_product(c3, 981.0f), 0); \
    const BwdifWords value = vec_sra(vec_sel(spatial, \
        vec_sra(hf, vec_splats(2u)) + lf, vec_cmpgt(bwdif_abs(c - e), delta)), \
        vec_splats(13u)); \
    result = vec_sel(vec_min(d + diff, vec_max(d - diff, value)), d, still); \
} while (0)
#define V(a) BWDIF_WIDEN(a, h, h)
            BWDIF_QUARTER(r0);
#undef V
#define V(a) BWDIF_WIDEN(a, h, l)
            BWDIF_QUARTER(r1);
#undef V
#define V(a) BWDIF_WIDEN(a, l, h)
            BWDIF_QUARTER(r2);
#undef V
#define V(a) BWDIF_WIDEN(a, l, l)
            BWDIF_QUARTER(r3);
#undef BWDIF_QUARTER
#undef V
            vec_st(vec_packsu(vec_packs(r0, r1), vec_packs(r2, r3)), x, dst);
        }
    }
    if (x < w)
        ff_bwdif_filter_line_c(dst + x, prev + x, cur + x, next + x, w - x,
                              p, m, p2, m2, p3, m3, p4, m4, parity, clip_max);
}
#undef BWDIF_WIDEN
