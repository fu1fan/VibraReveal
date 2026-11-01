# 08-docs · 设计与实验文档

跨模块的架构、设计决策、部署与实验记录。

| 子目录          | 内容                             | 状态                                   |
| --------------- | -------------------------------- | -------------------------------------- |
| `architecture/` | 数据流、模块职责、目标目录结构   | 已有 [overview.md](architecture/overview.md) |
| `validation/` | 实机性能、完整性、构建与交互验证 | 已有 USB/Metal、Vulkan/RKNN、Qt 触屏及无损录制记录 |

规划中：`deploy/`（构建、部署与回滚）、`experiments/`（实验记录）、`adr/`（架构决策记录）。

文档只描述跨模块约定与设计依据，单一模块的内部说明放在对应目录的 `README.md`。

## 当前入口（2026-11-01）

- [触屏主程序操作与构建](../03-apps/rk3588/TOUCH_APP.md)：调焦、选物、选区、放大、曝光、录制、回放及电脑直通。
- [板端综合验证](validation/rk3588-touch-2026-11-01.md)：Qt 5.15.10、真实相机/NPU、交互回归、120 FPS 档 eMMC 压缩录制及 Mac 兼容。
- [图像传输验收](validation/usb-bulk-images-2026-10-31.md)与[Metal 端到端验证](validation/usb-bulk-metal-2026-10-31.md)：接收零丢帧与处理队列替换分开统计。
- [VRI1 协议](../05-protocol/README.md)：原始传输与压缩归档格式、CRC 和完整结束条件。
- [双端录制早期设计](architecture/gui-recording-mac-design.md)：保留规划依据，首部列出与当前实现的差异。

2026-11-01 已将触屏测量与无损录制的 [PR #6](https://cnb.cool/fu1fan/VibraReveal/-/pulls/6) 合并至 `main`，合并提交 `763ac1a`。验收记录中的历史提交、哈希和测试日期保持原样；记录通过不代表尚未执行的长期压力、物理标定或人工触摸验收。
