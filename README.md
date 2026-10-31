# VibraReveal · 振见

**Intelligent Visual Motion Magnification & Vibration Sensing System**  
**智能视觉运动放大与非接触式振动感知系统**

VibraReveal（振见）是一个软硬件协同研发项目：利用 **OV9281 全局快门灰度摄像头**和 **RK3588 边缘计算平台**进行高帧率图像采集与运动预分析，并以 **Apple Silicon MacBook** 作为运动放大、振动频谱分析和交互可视化主机。

核心问题是：在保留原始图像时间分辨率的前提下，如何通过 GPU/NPU 协同、局部运动分析以及视觉—惯性信息融合，更可靠地发现、放大并解释微小运动。

> [!IMPORTANT]
> **系统与应用分别维护。** 本仓库只管理应用、算法、通信协议、整机硬件设计和实验资料。RK3588 的 Linux 系统、内核、设备树、OV9281 驱动、rootfs 和系统镜像由独立仓库 [RK3588-OV9281](https://cnb.cool/fu1fan/RK3588-OV9281) 维护。

> [!NOTE]
> **Work in Progress（2026-10-31）：** 已打通 OV9281 → RK3588 → USB Bulk → macOS Metal 命令行链路，并实现板端 Vulkan / RKNN 计算后端。全分辨率 120 FPS 图像传输通过约五分钟零丢帧验收；同条件 Metal 处理队列仍丢弃 0.1833% 输入帧，不能称为端到端零丢帧。图形界面、多后端并发、物理位移标定及整机长期稳定性仍待完成。

## 1. 项目目标

- **高帧率原始采集：** 面向 OV9281 `1280 × 800 @ 120 FPS`、RAW10 灰度视频，尽可能完整地保留帧编号、曝光信息和原始数据。
- **智能边缘分析：** 利用 RK3588 的 Vulkan Compute / NPU 辅助目标检测、ROI 管理、振动热点搜索和时空运动特征分析；预分析不得阻塞相机采集。
- **相位运动放大：** 在 MacBook 的 Metal GPU 上探索 Riesz Pyramid、Complex Steerable Pyramid 等方法，以及可选的学习式运动放大。
- **频率与空间分析：** 提取局部振动频谱、时频特征和空间振动分布，并区分运动信号和亮度变化干扰。
- **视觉惯性融合：** 研究六轴 IMU、相机自振、全局视觉配准和可选单目深度对运动补偿的辅助价值。
- **工程可复现：** 独立构建应用、独立发布系统，统一管理采购模块、原生 CAD、电子设计、标定数据和实验记录。

## 2. 双仓库分工

| 仓库                                                       | 维护内容                                                     | 典型产物                               |
| ---------------------------------------------------------- | ------------------------------------------------------------ | -------------------------------------- |
| **[RK3588-OV9281](https://cnb.cool/fu1fan/RK3588-OV9281)** | ELF2/RK3588 板级支持、Buildroot、Linux 内核及设备树、OV9281 驱动、基础系统服务、GPU/NPU 驱动与运行时 | `update.img`、按 digest 固定的版本化应用 SDK |
| **VibraReveal（本仓库）**                                  | RK3588 Linux 用户态应用、Vulkan Shader、Mac Metal 应用、算法、设备通信、原型机械/电子设计与实验 | ARM64 应用包、Mac 应用、协议与设计文件 |

> [!NOTE]
> `RK3588-OV9281` 以 Git 子模块形式挂载在 [02-platform/RK3588-OV9281](02-platform/RK3588-OV9281)，本仓库只跟踪其提交指针，不修改子模块内容。

## 3. 仓库目录

```text
VibraReveal/
├── 01-hardware/       # 采集与计算硬件
│   ├── modules/       # 已采购模块建档（7inch_HDMI_LCD_C、elf2、icm-42688-p、ov9281）
│   └── mechanical/    # 整机机械设计 V7.3：src 源 / drawings+assembly 交付 / build 产物
├── 02-platform/       # 平台环境；含 RK3588-OV9281 子模块
├── 03-apps/
│   ├── rk3588/        # RK3588 Linux 用户态应用
│   └── macos/         # Swift/C++、Metal、交互可视化
├── 04-algorithms/     # Python/PyTorch 参考实现及模型转换
├── 05-protocol/       # RAW、IMU、ROI、控制消息与版本协议
├── 06-tests/          # 跨模块集成测试、性能及硬件验收
├── 07-datasets/       # 小型样本、数据索引与校验值
├── 08-docs/           # 架构、设计、部署与实验文档
├── 09-tools/          # 构建、部署和辅助工具
└── 10-env/            # 日常 IDE、按需应用 SDK 与完整系统环境入口
```

顶层目录按研发顺序编号；`03-apps/rk3588/` 包含采集、USB Bulk 传输、Vulkan 放大/特征与 RKNN 选物后端，`03-apps/macos/` 包含 Swift 接收器与 Metal 实时/离线放大后端；`04-algorithms/phase-motion/` 提供 Python 参考实现，`06-tests/` 与 `08-docs/validation/` 保存回归脚本和验收证据。硬件资料与系统子模块分别位于 `01-hardware/` 和 `02-platform/`。

`10-env/` 提供轻量日常镜像（Python、code-server）和独立完整系统环境。应用 SDK 从系统流水线产物按固定 digest 加载；日常镜像支持页面按钮、API 和 `dev-env-v*` Tag 发布，用法见 [10-env/README.md](10-env/README.md)。

## 4. 硬件与模块资料

已采购模块按型号独立建档，资料放在 [01-hardware/modules](01-hardware/modules)：

| 模块                                        | 用途                       | 已归档内容                                                                 |
| ------------------------------------------- | -------------------------- | -------------------------------------------------------------------------- |
| [ov9281](01-hardware/modules/ov9281)         | 全局快门灰度相机           | 传感器数据手册、15P 摄像头模组结构图                                       |
| [elf2](01-hardware/modules/elf2)             | ELF 2（RK3588）开发板      | 产品规格书、快速使用手册、软硬件教程、V1.3 原理图与 Altium 源文件、芯片手册、引脚复用表、结构图纸 |
| [7inch_HDMI_LCD_C](01-hardware/modules/7inch_HDMI_LCD_C) | 7 寸 HDMI 显示模组 | 结构 STEP/PRT 模型                                                         |
| [icm-42688-p](01-hardware/modules/icm-42688-p) | 六轴 IMU（相机自振参考与时基对齐） | 芯片数据手册、模块原理图、厂商说明、STM32 示例固件                         |

各模块 `README.md` 记录采购来源。

## 5. 当前进度与路线图

截至 **2026-10-31**，已完成能力与验证边界如下：

| 能力 | 当前结果 | 验收记录 |
| --- | --- | --- |
| 相机与 USB Bulk 传输 | 1280×800 Y10，36,000 帧约 298.5 秒，零序号缺口/发送丢弃；离线最高约 417.4 MB/s | [图像传输](08-docs/validation/usb-bulk-images-2026-10-31.md) |
| Mac Metal 实时放大 | 同尺寸完成 35,934/36,000 帧，队列替换 66 帧；估算采集至结果 P50/P95 为 17.865/20.005 ms | [处理延迟与丢帧](08-docs/validation/usb-bulk-metal-2026-10-31.md) |
| 板端 Vulkan 实时 ROI | 128×128@120、256×256@60 短测通过；全帧@120 超预算并明确报错 | [ROI 短测](08-docs/validation/vulkan-roi-motion-2026-10-31.md) |
| GPU 对焦/边缘与 NPU 选物 | Mali 特征计算与 MobileSAM encoder/decoder 实机通过；分割质量仅验证公开灰度示例 | [五项后端](08-docs/validation/rk3588-backends-2026-10-31.md) |
| 整段离线全帧放大 | 默认前后向零相位滤波，保留全部帧与时间信息；480 帧夹具 Mali/Metal 最大灰度误差约 2.38×10⁻⁶ | [零相位对照](08-docs/validation/rk3588-offline-filter-2026-10-31.md) |

上述吞吐和延迟依赖记录中的设备、负载与测试时长；数值一致性不等于物理位移精度标定。USB 完整接收与算法处理零丢帧分别验收，短时通过不代表全天运行或多后端并发达标。

- [x] 确定 OV9281 + RK3588 采集路线，完成驱动适配、首次采集帧率修复及板端验证
- [x] 维护独立系统仓库与子模块，建立固定 digest 的应用交叉编译 SDK
- [x] 建立独立应用构建、传输服务部署/恢复及可搬迁后端包
- [x] 完成高速 RAW 传输、CRC/序号/END 校验、过载失败与重连验收
- [x] 实现 VRI1 帧编号、单调时间、位深、stride 与完整性协议
- [x] 整理 [Python 相位运动放大与频谱参考](04-algorithms/phase-motion/)
- [x] 实现 [Mac Metal 实时/离线后端](03-apps/macos/)，完成真实 Bulk 输入短期验收
- [x] 实现 [RK3588 五项后端](03-apps/rk3588/BACKENDS.md)及整段零相位离线处理
- [x] 归档 ELF 2、OV9281、HDMI 屏与 IMU 模块资料
- [ ] 完成热点扫描、振动频谱预分析与跨帧 ROI 跟踪
- [ ] 完成多后端并发调度、端到端零丢帧优化和持续负载验收
- [ ] 集成图形界面、视频编解码与可选深度估计
- [ ] 补充曝光元数据、IMU 数据协议与时间同步，完成自振检测/视觉辅助补偿
- [ ] 完成机械与电子集成、真实位移标定、服务冷启动及整机长期稳定性验收

## 6. 文档索引

| 内容                            | 位置                                                                     |
| ------------------------------- | ------------------------------------------------------------------------ |
| RK3588 采集与传输 | [03-apps/rk3588/README.md](03-apps/rk3588/README.md) |
| Vulkan / RKNN 后端与模型转换 | [03-apps/rk3588/BACKENDS.md](03-apps/rk3588/BACKENDS.md) |
| Mac Metal 后端与使用方式 | [03-apps/macos/README.md](03-apps/macos/README.md) |
| VRI1 图像传输协议 | [05-protocol/README.md](05-protocol/README.md) |
| 回归测试与硬件验收入口 | [06-tests/README.md](06-tests/README.md) |
| Python 算法安装、运行、数据格式 | [04-algorithms/phase-motion/README.md](04-algorithms/phase-motion/README.md) |
| 开发环境镜像、ELF2 构建与交叉编译、IDE 状态持久化 | [10-env/README.md](10-env/README.md) |
| 数据流、模块职责、目标目录结构  | [08-docs/architecture/overview.md](08-docs/architecture/overview.md)       |

## 7. 许可证

本仓库源码以 [MIT 许可证](LICENSE) 发布，Copyright (c) 2026 fu1fan。

`01-hardware/` 中的厂商资料（数据手册、参考原理图、CAD 文件等）版权归各自厂商所有，不在 MIT 许可范围内。OV9281/RK3588 系统仓库、Rockchip SDK、第三方模型及厂商文档分别遵循其自身许可；引入算法代码或预训练权重时，请保存上游仓库、提交版本及许可说明。

---

**VibraReveal · 振见** — *让不可见的振动，被看见。*

日常环境与完整系统构建环境已分离：使用 `elf2-cmake` 按需加载固定版本应用 SDK，
使用 `elf2-system` 进入完整系统环境。详见 [环境与 SDK 指南](10-env/README.md)。
