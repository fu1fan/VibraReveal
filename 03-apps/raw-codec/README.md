# Shared lossless RAW codec

Shared C codec for RK3588 and macOS. Codec 1 uses Bitshuffle on fixed 8192-byte blocks (element size 1 for RAW8, 2 for RAW10/12/16), then LZ4 block encoding at acceleration 8. No quantization, masking or inter-frame dependencies. Incompressible frames use codec 0 (exact raw bytes). CRC covers decoded bytes. Decoder buffers and input lengths are bounded by original frame size.

Pinned, unmodified sources:
- LZ4 v1.10.0: https://github.com/lz4/lz4/tree/v1.10.0/lib (BSD-2-Clause, vendor/lz4/LICENSE).
- Bitshuffle commit 54b949450d3c8fbac57d7968d4a4c35861cc038a: https://github.com/kiyo-masui/bitshuffle/tree/54b949450d3c8fbac57d7968d4a4c35861cc038a/src (MIT, vendor/bitshuffle/LICENSE).

No OpenMP or global CPU configuration is used. The vendored implementation selects NEON on ARM when available. Archive fields are documented in ../../05-protocol/README.md.
