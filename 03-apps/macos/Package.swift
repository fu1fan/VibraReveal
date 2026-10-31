// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "VibraReveal",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "VibraRevealBackend", targets: ["VibraRevealBackend"]),
        .executable(name: "vibrareveal", targets: ["vibrareveal"]),
        .library(name: "VibraRevealTransport", targets: ["VibraRevealTransport"]),
        .executable(name: "vr-receive", targets: ["vr-receive"])
    ],
    targets: [
        .target(name: "VibraRevealBackend", resources: [.copy("Resources/Motion.metal")]),
        .target(name: "VibraRevealTransport", dependencies: ["VibraRevealBackend"]),
        .executableTarget(name: "vr-receive", dependencies: ["VibraRevealTransport"]),
        .executableTarget(name: "vibrareveal", dependencies: ["VibraRevealBackend", "VibraRevealTransport"]),
        .testTarget(name: "VibraRevealTransportTests", dependencies: ["VibraRevealTransport"]),
        .testTarget(name: "VibraRevealBackendTests", dependencies: ["VibraRevealBackend"])
    ]
)
