# 06-tests · 集成测试与验收

跨模块的集成测试、性能基准与硬件验收记录。

## macOS 后端

- Swift/Metal 单元与合成算法测试：`swift test --package-path 03-apps/macos`（从仓库根目录执行）。
- CLI 二进制数据流集成检查：`python3 06-tests/macos_backend_cli.py /path/to/vibrareveal`。
- 测试内容和本机性能记录：[macOS VALIDATION](../03-apps/macos/VALIDATION.md)。

真实采集、传输及持续运行验收仍需单独完成。

## 图像传输

- 跨语言协议/调度/异常集成：`python3 06-tests/image_transport.py /path/vr-stream-host /path/vr-receive /path/vibrareveal`。
- Swift 协议测试包含在 `swift test` 中。
- [硬件验收记录](../08-docs/validation/image-transport-2026-10-31.md) 区分已通过的检查与待验收项。
