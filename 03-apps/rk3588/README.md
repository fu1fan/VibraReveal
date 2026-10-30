# 03-apps/rk3588 · RK3588 Linux 用户态应用

在 ELF 2（RK3588）上运行的采集与边缘预分析应用。

## 职责

- V4L2 采集 OV9281 RAW10 数据，保留帧序号、曝光与时间戳
- 帧缓冲与异步队列，预分析不得阻塞采集
- Vulkan Compute 局部运动特征与热点扫描
- NPU（RKNN）目标检测与 ROI 跟踪
- IMU 采样与时间对齐
- 面向 MacBook 的高速传输与控制通道

## 当前状态

仅目录占位，尚无代码。目标结构见 [系统架构概览](../../08-docs/architecture/overview.md)。

## 开发环境

交叉编译工具链、sysroot 与 ELF2 增量构建缓存由 [10-env](../../10-env/README.md) 的镜像提供。在云原生开发环境里登录 shell 会自动加载 `/etc/profile.d/elf2.sh`：

```bash
make CROSS_COMPILE="$ELF2_CROSS_COMPILE" ARCH=arm64
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/opt/elf2/cmake/elf2-aarch64-toolchain.cmake
```

## 边界

Linux 内核、设备树、OV9281 驱动、Buildroot 与系统镜像不在本目录维护，见独立仓库 [RK3588-OV9281](https://cnb.cool/fu1fan/RK3588-OV9281)。
