// swift-tools-version: 5.9
import PackageDescription
let package = Package(name: "VRRawCodec", products: [.library(name: "CVRRawCodec", targets: ["CVRRawCodec"])],
    targets: [.target(name: "CVRRawCodec", path: ".", exclude: ["CMakeLists.txt", "README.md"],
        sources: ["src/vr_raw_codec.c", "vendor/lz4/lz4.c", "vendor/bitshuffle/bitshuffle_core.c", "vendor/bitshuffle/iochain.c"],
        publicHeadersPath: "include", cSettings: [.headerSearchPath("vendor/lz4"), .headerSearchPath("vendor/bitshuffle")])], cLanguageStandard: .c99)
