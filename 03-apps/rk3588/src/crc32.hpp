#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#if defined(__aarch64__) && defined(__linux__)
#include <arm_acle.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
__attribute__((target("arch=armv8-a+crc")))
inline uint32_t hardwareCRC(const uint8_t* p, size_t n) {
    uint32_t c=0xffffffff;
    while(n>=8) { uint64_t word; memcpy(&word,p,8); c=__crc32d(c,word); p+=8; n-=8; }
    while(n--) c=__crc32b(c,*p++);
    return ~c;
}
#endif
// IEEE CRC32, compatible with zlib. Slicing-by-eight fallback has no SDK dependency.
inline uint32_t frameCRC(const uint8_t* p, size_t n) {
#if defined(__aarch64__) && defined(__linux__)
    if(getauxval(AT_HWCAP)&HWCAP_CRC32) return hardwareCRC(p,n);
#endif
    static const auto table=[] {
        std::array<std::array<uint32_t,256>,8> t{};
        for(uint32_t i=0;i<256;++i) {
            uint32_t c=i;
            for(int b=0;b<8;++b) c=(c>>1)^((c&1)?0xedb88320u:0);
            t[0][i]=c;
        }
        for(int k=1;k<8;++k) for(int i=0;i<256;++i) t[k][i]=(t[k-1][i]>>8)^t[0][t[k-1][i]&255];
        return t;
    }();
    uint32_t c=0xffffffff;
    while(n>=8) {
        uint32_t v=c^uint32_t(p[0])^(uint32_t(p[1])<<8)^(uint32_t(p[2])<<16)^(uint32_t(p[3])<<24);
        c=table[7][v&255]^table[6][(v>>8)&255]^table[5][(v>>16)&255]^table[4][v>>24]
         ^table[3][p[4]]^table[2][p[5]]^table[1][p[6]]^table[0][p[7]];
        p+=8; n-=8;
    }
    while(n--) c=(c>>8)^table[0][(c^*p++)&255];
    return ~c;
}
