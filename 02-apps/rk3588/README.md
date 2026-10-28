# 02-apps/rk3588 · RK3588 Linux 用户态应用

在 ELF 2（RK3588）上运行的采集与边缘预分析应用。

## 职责

- V4L2 采集 OV9281 RAW10 数据，保留帧序号、曝光与时间戳
- 帧缓冲与异步队列，预分析不得阻塞采集
- Vulkan Compute 局部运动特征与热点扫描
- NPU（RKNN）目标检测与 ROI 跟踪
- IMU 采样与时间对齐
- 面向 MacBook 的高速传输与控制通道

## 当前状态

仅目录占位，尚无代码。目标结构见 [系统架构概览](../../07-docs/architecture/overview.md)。

## 边界

Linux 内核、设备树、OV9281 驱动、Buildroot 与系统镜像不在本目录维护，见独立仓库 [RK3588-OV9281](https://cnb.cool/fu1fan/RK3588-OV9281)。
