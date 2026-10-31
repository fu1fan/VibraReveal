# 06-tests · 集成测试与验收

跨模块的集成测试、性能基准与硬件验收记录。

## macOS 后端

- Swift/Metal 单元与合成算法测试：`swift test --package-path 03-apps/macos`（从仓库根目录执行）。
- CLI 二进制数据流集成检查：`python3 06-tests/macos_backend_cli.py /path/to/vibrareveal`。
- 测试内容和本机性能记录：[macOS VALIDATION](../03-apps/macos/VALIDATION.md)。

真实 Bulk 采集与 Metal 处理已有约五分钟验收，见下文；长期稳定性及真实位移标定仍需单独完成。

## 图像传输

- 跨语言协议/调度/异常集成：`python3 06-tests/image_transport.py /path/vr-stream-host /path/vr-receive /path/vibrareveal`。
- Swift 协议测试包含在 `swift test` 中。
- [硬件验收记录](../08-docs/validation/image-transport-2026-10-31.md) 区分已通过的检查与待验收项。

## 正式 USB Bulk 图像通道

先经 Wi-Fi 停止 `/etc/init.d/S91vrstream` 服务，再运行：

```sh
python3 06-tests/usb-bulk/image_acceptance.py --board root@192.168.8.204 \
  --ssh-control /tmp/vr-bulk-ssh --receiver /path/to/vr-receive --frames 36000 \
  --offline-raw /tmp/recording.raw --output /tmp/bulk-acceptance
```

脚本通过 Wi-Fi SSH 启动 CNB 已编译、已部署的发送器；图像只经自定义 USB Bulk。
包括连续采集、暂停接收 100 ms、过载 800 ms 明确失败、失败后重连及三次离线吞吐。
离线输入应为完整 1280×800 Y10（stride=2560）RAW；脚本不会创建或覆盖板端输入文件。
最大链路测试使用 tmpfs/缓存文件，输出仅校验不落盘；存储端到端性能另测。

## USB Bulk → Metal 实机延迟与丢帧

板端时钟探测器必须使用 CNB 制品中的 `/vr-clock-probe`，经 Wi-Fi 安装到 `/usr/local/bin/vr-clock-probe`。
停止 `/etc/init.d/S91vrstream` 后执行：

```sh
python3 06-tests/usb-bulk/metal_acceptance.py --board root@192.168.8.204 \
  --ssh-control /tmp/vr-bulk-ssh --binary /path/to/vibrareveal \
  --frames 36000 --output /tmp/metal-live
```

固定 1280×800、120 fps 源、alpha=10、三层、3–12 Hz，禁止自适应缩放。
默认不序列化像素输出，但完整执行 Float32 读回；`--write-output` 对照测试输出到 `/dev/null`。
每次使用新的输出目录。得到逐帧遥测、发送/接收日志、时钟样本和分阶段延迟分位数。
脚本断言 USB 接收完整和所有帧处理尺寸不变；**算法丢帧是被测结果，不要求为零**。
结束后按需恢复 `/etc/init.d/S91vrstream start`。不要并行运行其他 Metal 测试，避免干扰结果。

## RK3588 Vulkan / RKNN 后端

构建选项与依赖见 [后端接口](../03-apps/rk3588/BACKENDS.md)。在具备 Vulkan 的 Linux 环境运行：

```sh
ctest --test-dir /path/to/backend-build --output-on-failure
python3 06-tests/vulkan_motion.py /path/to/backend-build/motion-test.f32
python3 06-tests/vulkan_motion_cli.py /path/to/backend-build/vr-motion /path/to/backend-build/motion.comp.spv
python3 06-tests/rk3588_backend.py /path/to/backend-build
```

- [五项后端验收](../08-docs/validation/rk3588-backends-2026-10-31.md)：特征提取、交互选物与录像完整性。
- [零相位验收与复现](../08-docs/validation/rk3588-offline-filter-2026-10-31.md)：`offline_filter.py` 的 prepare/check/compare 流程及 Metal/Mali 对照。
- [实时 ROI 验收](../08-docs/validation/vulkan-roi-motion-2026-10-31.md)：实际相机短测与超预算失败。

RKNN 选择测试需要实际 NPU、对应模型和运行时，按后端文档显式运行；不作为无硬件环境的 CTest 项。
