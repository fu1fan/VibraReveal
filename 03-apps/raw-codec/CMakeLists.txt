cmake_minimum_required(VERSION 3.16)
project(VRRawCodec LANGUAGES C)
add_library(vr-raw-codec STATIC src/vr_raw_codec.c vendor/lz4/lz4.c
    vendor/bitshuffle/bitshuffle_core.c vendor/bitshuffle/iochain.c)
target_include_directories(vr-raw-codec PUBLIC include PRIVATE vendor/lz4 vendor/bitshuffle)
set_target_properties(vr-raw-codec PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
install(TARGETS vr-raw-codec ARCHIVE DESTINATION lib)
install(FILES include/vr_raw_codec.h DESTINATION include/vibrareveal)
