#include "vr_raw_codec.h"
#include "lz4.h"
#include "bitshuffle_core.h"
static int valid(size_t size,unsigned bits) {
    return size>0 && size<=256u*1024u*1024u && (bits==8 || bits==10 || bits==12 || bits==16);
}
int vr_raw_encode(const void *raw,size_t size,unsigned bits,void *scratch,void *output) {
    if(!raw || !scratch || !output || !valid(size,bits)) return -1;
    const size_t element=bits==8?1:2;
    if(size%element || size<16) return 0;
    if(bshuf_bitshuffle(raw,scratch,size/element,element,8192/element)!=(int64_t)size) return -1;
    int n=LZ4_compress_fast((const char *)scratch,(char *)output,(int)size,(int)size,8);
    return n>0 && n<(int)size ? n : 0;
}
int vr_raw_decode(const void *encoded,size_t encoded_size,size_t size,unsigned bits,void *scratch,void *output) {
    if(!encoded || !scratch || !output || !valid(size,bits) || !encoded_size || encoded_size>=size) return -1;
    const size_t element=bits==8?1:2;
    if(size%element) return -1;
    if(LZ4_decompress_safe((const char *)encoded,(char *)scratch,(int)encoded_size,(int)size)!=(int)size) return -1;
    return bshuf_bitunshuffle(scratch,output,size/element,element,8192/element)==(int64_t)size ? 0 : -1;
}
