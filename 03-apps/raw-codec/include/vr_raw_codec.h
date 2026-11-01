#ifndef VR_RAW_CODEC_H
#define VR_RAW_CODEC_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Codec 1: Bitshuffle (fixed 8192-byte blocks), then independent LZ4 at
 * acceleration 8. Preserves every byte, including padding and unused bits.
 * Scratch/output each need raw_size bytes and must not overlap.
 * Encode: stored bytes, 0 = keep raw, -1 = error. Decode: 0 = success, -1 = error. */
int vr_raw_encode(const void *raw,size_t raw_size,unsigned bits,void *scratch,void *output);
int vr_raw_decode(const void *encoded,size_t encoded_size,size_t raw_size,unsigned bits,void *scratch,void *output);
#ifdef __cplusplus
}
#endif
#endif
