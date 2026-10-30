// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "VibraReveal",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "VibraRevealBackend", targets: ["VibraRevealBackend"]),
        .executable(name: "vibrareveal", targets: ["vibrareveal"])
    ],
    targets: [
        .target(name: "VibraRevealBackend", resources: [.copy("Resources/Motion.metal")]),
        .executableTarget(name: "vibrareveal", dependencies: ["VibraRevealBackend"]),
        .testTarget(name: "VibraRevealBackendTests", dependencies: ["VibraRevealBackend"])
    ]
)
