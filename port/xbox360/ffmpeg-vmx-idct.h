#ifndef DVD360_FFMPEG_VMX_IDCT_H
#define DVD360_FFMPEG_VMX_IDCT_H

#include <altivec.h>

// Split a into signed high and unsigned low bytes. Both partial products
// fit in 24 bits, so the floating-point multiplies are exact.
static inline __vector signed int dvd360_mul_short_words(
    __vector signed int a, __vector signed int b)
{
    const __vector unsigned int bits = vec_splats(8u);
    const __vector signed int low = vec_and(a, vec_splats(255));
    const __vector float bf = vec_ctf(b, 0);
    const __vector float zero = vec_splats(0.0f);
    const __vector signed int hi_product =
        vec_cts(vec_madd(vec_ctf(vec_sra(a, bits), 0), bf, zero), 0);
    const __vector signed int lo_product =
        vec_cts(vec_madd(vec_ctf(low, 0), bf, zero), 0);
    return vec_add(vec_sl(hi_product, bits), lo_product);
}

// VMHRADDSHS: saturate signed (c + ((a*b + 0x4000) >> 15)).
static inline __vector signed short dvd360_vec_mradds(
    __vector signed short a, __vector signed short b, __vector signed short c)
{
    const __vector signed int round = vec_splats(0x4000);
    const __vector unsigned int shift = vec_splats(15u);
    const __vector signed int hi = vec_add(vec_unpackh(c), vec_sra(
        vec_add(dvd360_mul_short_words(vec_unpackh(a), vec_unpackh(b)), round), shift));
    const __vector signed int lo = vec_add(vec_unpackl(c), vec_sra(
        vec_add(dvd360_mul_short_words(vec_unpackl(a), vec_unpackl(b)), round), shift));
    return vec_packs(hi, lo);
}

#define vec_mradds dvd360_vec_mradds
#endif
