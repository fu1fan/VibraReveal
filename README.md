# VibraReveal · 振见

**Intelligent Visual Motion Magnification & Vibration Sensing System**  
**智能视觉运动放大与非接触式振动感知系统**

VibraReveal（振见）是一个软硬件协同研发项目，利用 **OV9281 全局快门灰度摄像头**和 **RK3588 边缘计算平台**进行高帧率图像采集与运动预分析，并以 **Apple Silicon MacBook** 作为运动放大、振动频谱分析和交互可视化主机。

项目研究的核心问题是：在保留原始图像时间分辨率的同时，如何通过 GPU/NPU 协同、局部运动分析以及视觉—惯性信息融合，更可靠地发现、放大并解释微小运动。

> [!IMPORTANT]
> **系统与应用分别维护。** 本仓库只管理 VibraReveal 的应用程序、算法、通信协议、整机硬件设计和实验资料。RK3588 的 Linux 系统、内核、设备树、OV9281 驱动、基础 rootfs 和系统镜像由独立仓库 [RK3588-OV9281](https://cnb.cool/fu1fan/RK3588-OV9281) 维护。

> [!NOTE]
> **Work in Progress：** 本文描述目标架构，尚未建立的目录和功能均为规划项。驱动与 Vulkan 环境的已有进展，不代表端到端高速传输、实时运动放大或多传感器同步已经通过验收。

## 1. 项目目标

- **高帧率原始采集：** 面向 OV9281 `1280 × 800 @ 120 FPS`、RAW10 灰度视频，尽可能完整地保留帧编号、曝光信息和原始数据。
- **智能边缘分析：** 利用 RK3588 的 Vulkan Compute / NPU 辅助目标检测、ROI 管理、振动热点搜索和时空运动特征分析；预分析不得阻塞相机采集。
- **相位运动放大：** 在 MacBook 的 Metal GPU 上探索 Riesz Pyramid、Complex Steerable Pyramid 等方法，以及可选的学习式运动放大。
- **频率与空间分析：** 对选定目标提取局部振动频谱、时频特征和空间振动分布，并区分运动信号和亮度变化干扰。
- **视觉惯性融合：** 研究六轴 IMU、相机自振、全局视觉配准和可选单目深度对运动补偿的辅助价值。
- **工程可复现：** 独立构建应用、独立发布系统，统一管理采购模块、原生 CAD、电子设计、标定数据和实验记录。

## 2. 系统架构

```text
          OV9281 global-shutter camera
             RAW10 · up to 120 FPS
                       │ MIPI CSI-2
                       ▼
     ┌──────────────────────────────────────┐
     │ RK3588 · Linux edge device           │
     │                                      │
IMU ─┤ V4L2 capture / frame timestamps      │
     │ Vulkan compute / motion pre-analysis │
     │ RKNN inference / ROI tracking        │
     │ buffering / transport / control      │
     └──────────────────┬───────────────────┘
                        │ High-speed link (under development)
                        │ RAW frames + timestamps + metadata
                        ▼
     ┌──────────────────────────────────────┐
     │ MacBook · Apple Silicon              │
     │ Metal motion magnification           │
     │ spectral analysis / heatmaps / UI    │
     └──────────────────────────────────────┘
```

**基本约束：** 图像采集和传输为关键路径；GPU、NPU、深度估计与热点分析应异步执行。所有衍生结果必须关联原始帧编号及对应采样时间。预分析结果是可撤销的建议，不能取代原始图像。

## 3. 双仓库分工

| 仓库                                                       | 维护内容                                                     | 典型产物                               |
| ---------------------------------------------------------- | ------------------------------------------------------------ | -------------------------------------- |
| **[RK3588-OV9281](https://cnb.cool/fu1fan/RK3588-OV9281)** | ELF2/RK3588 板级支持、Buildroot、Linux 内核及设备树、OV9281 驱动、基础系统服务、GPU/NPU 驱动与运行时 | `update.img`；未来可提供版本化应用 SDK |
| **VibraReveal（本仓库）**                                  | RK3588 Linux 用户态应用、Vulkan Shader、Mac Metal 应用、算法、设备通信、原型机械/电子设计与实验 | ARM64 应用包、Mac 应用、协议与设计文件 |

## 4. 仓库目录

```text
VibraReveal/
├── apps/
│   ├── edge/                    # RK3588 Linux 用户态应用
│   │   ├── src/
│   │   │   ├── capture/         # V4L2、RAW10、帧缓冲
│   │   │   ├── transport/       # 高速图像传输与控制
│   │   │   ├── sensors/         # IMU 与其他传感器采样
│   │   │   ├── sync/            # 时钟域、时间戳与同步
│   │   │   ├── compute/         # GPU/NPU 计算调度
│   │   │   └── pipeline/        # 异步流水线与队列
│   │   ├── shaders/             # Vulkan Compute / GLSL / SPIR-V
│   │   ├── include/
│   │   ├── config/
│   │   ├── tests/
│   │   └── CMakeLists.txt
│   └── macos/                   # Swift/C++、Metal、交互可视化
├── algorithms/                  # Python/PyTorch 参考实现及模型转换
├── protocol/                    # RAW、IMU、ROI、控制消息与版本协议
├── hardware/
│   ├── modules/                 # 已采购模块资料和来源
│   ├── electronics/             # 原理图、PCB、接线和线束
│   ├── mechanical/              # 原生 CAD、零件、STEP/STP、STL
│   └── assembly/                # 整机装配、BOM、工艺与制造输出
├── packaging/
│   └── edge/                    # 安装脚本、启动服务、打包元数据
├── toolchains/                  # 交叉编译配置，不存放完整 SDK
├── tests/                       # 跨模块集成测试、性能及硬件验收
├── datasets/                    # 小型样本、数据索引与校验值
├── docs/
│   ├── architecture/            # 系统、软硬件边界和数据流
│   ├── design/                  # 时间同步、振动分析、GPU流水线
│   ├── deployment/              # 系统依赖、应用安装、回滚
│   ├── guides/                  # 装配、环境配置与标定
│   ├── experiments/             # 实验方法、结果摘要
│   └── decisions/               # 架构决策记录（ADR）
├── scripts/                     # 构建、部署和辅助工具
├── .cnb.yml                     # 计划中的应用 CI/CD 配置
├── .gitattributes
├── .gitignore
└── README.md
```

**目录为目标结构，尚未实现的功能无需为了凑齐目录而创建空代码。** Linux 内核、Buildroot、设备树和系统镜像均不属于 `apps/edge/`。

## 5. 硬件资料与设计文件

### 采购模块：`hardware/modules/`

每个实际采购模块独立建档，按 **类别-完整型号** 命名，例：

```text
hardware/modules/
├── bom.csv                       # 模块汇总与采购信息
├── camera-ov9281/
│   ├── README.md                 # 厂商、具体料号、硬件修订、来源
│   ├── datasheets/               # 可合法存储的原厂资料
│   ├── schematics/               # 公开原理图
│   ├── pinout/                   # 引脚、电压、接口定义
│   ├── mechanical/               # 安装尺寸、3D模型
│   └── images/                   # 实物和装配照片
├── sbc-rk3588/
└── imu-icm42688/                 # 候选器件，采购后按实物建档
```

模块的 `README.md` 至少记录完整商品型号、厂商、硬件版本、采购链接、资料来源、接口、电气限制、安装尺寸及当前验证状态。**OV9281传感器芯片的数据手册不能替代具体摄像头模组的引脚与机械资料。** 受许可限制的原厂文档仅记录来源与校验值，原件保存在授权位置。

### 自研硬件：`hardware/electronics/` 与 `hardware/mechanical/`

- **电子：** 保留 KiCad 等原始工程、元件库、接线图、接口板、制造输出；原理图与 PCB 文件为设计源，Gerber 为发布产物。
- **机械：** 保留 FreeCAD/Fusion 等原生可编辑工程；`*.step`/`*.stp` 作为通用三维交换格式，`*.stl`/`*.3mf` 用于制造，工程图和装配文件单独归档。
- **装配：** 在 `hardware/assembly/` 记录整机 BOM、固定件、线束、相机光轴与 IMU 坐标系，以及刚性安装关系。

## 6. 软件模块与计算职责

| 模块           | 平台                      | 职责                                      | 状态                          |
| -------------- | ------------------------- | ----------------------------------------- | ----------------------------- |
| 图像采集与缓冲 | RK3588 CPU / V4L2         | RAW10、帧序号、曝光与队列                 | 驱动基础已有；应用待集成      |
| 边缘运动分析   | RK3588 Mali-G610 / Vulkan | 局部相位代理、连续时域特征、Tile 热点扫描 | Vulkan 环境已调通；算法待实现 |
| 场景感知       | RK3588 NPU / RKNN         | 目标检测、ROI、可选深度与学习式特征       | 规划中                        |
| IMU采集及对齐  | RK3588 / SPI              | 自振频谱、时间基准与同步                  | 规划中                        |
| 高速传输       | RK3588 ↔ MacBook          | RAW数据、时间戳、ROI与控制                | 待联调及压力测试              |
| 相位运动放大   | MacBook M5 / Metal        | 时域滤波、金字塔、放大和重建              | 规划中                        |
| 测量与可视化   | MacBook                   | 频谱、时频图、热力图、选区操作            | 规划中                        |

分块预分析保留原始像素尺寸时，仍应保证对应Tile的**时间采样连续性**。逐帧轮流切换Tile虽然不会降低相机采集FPS，却会降低该Tile的有效时间采样率；需要采用连续特征采样或缓存连续原始帧后异步分析。

## 7. 构建、部署与系统兼容

### 7.1 系统镜像（独立仓库）

系统构建、依赖归档、`system-v*` 标签触发的CNB流程、镜像输出及现有验证限制，请以 **[RK3588-OV9281 README](https://cnb.cool/fu1fan/RK3588-OV9281/-/blob/main/README.md)** 与其 `CI.md` 为准。

该仓库目前使用已有的预编译 rootfs 基线；其文档注明部分最新修复尚未在真实开发板上验收。本仓库不会重复维护这些系统构建步骤或声称镜像已完成板级验收。

### 7.2 RK3588应用（本仓库，计划建立）

推荐使用 **CMake + Ninja + 与目标系统匹配的 AArch64 交叉编译工具链/sysroot**，由 CNB 构建应用包；Vulkan Shader 在构建主机上编译为 SPIR-V。拟建立的开发流程为：

1. 在系统仓库固定一个已验收的系统版本，导出或整理匹配的应用SDK（工具链、sysroot、开发头文件及版本清单）。
2. 本仓库通过 `toolchains/` 中的CMake工具链配置引用SDK，而不是复制整套系统源码。
3. CNB执行交叉编译、Shader验证、协议/单元测试，输出独立的ARM64安装包。
4. 在目标开发板上安装到独立应用目录，验证V4L2、Vulkan、USB、RKNN等运行时依赖。
5. 使用版本目录和原子切换实现应用升级与回滚，**无需每次重新刷写系统镜像**。

**目前尚未发布可直接使用的统一SDK、交叉编译命令或安装包**；对应脚本实现并验证后，将在 `apps/edge/README.md` 与 `docs/deployment/` 中提供可复制的构建和部署命令。

应用安装布局建议：

```text
/opt/vibrareveal/releases/<version>/   # 独立版本文件
/opt/vibrareveal/current               # 当前版本符号链接
/etc/vibrareveal/                       # 运行配置
/var/lib/vibrareveal/                   # 可写运行状态
```

上述布局的前提是目标系统具有合适的可写分区、空间和启动机制；正式实施前应在板端核实。

### 7.3 建议的版本管理

- 系统仓库：`system-v*`，发布系统镜像；未来配套SDK及其ABI/依赖清单。
- 本仓库边缘应用：计划使用 `edge-v*`，发布ARM64应用包。
- Mac客户端：采用独立应用版本号；通信协议另设`protocol_version`。
- 应用发布物记录依赖的系统版本、编译SDK版本、架构以及关键Vulkan/RKNN能力；**版本匹配以实际测试为准**。

云端CI适合验证交叉编译、SPIR-V、协议与可移植单元测试；**Mali GPU吞吐、OV9281采集、持续RAW传输、IMU同步精度和Metal性能必须在真实硬件上测量**。

## 8. 当前进度与路线图

- [x] 确定OV9281 + RK3588的高速灰度采集路线
- [x] 维护独立的 [RK3588-OV9281](https://cnb.cool/fu1fan/RK3588-OV9281) 系统源码/构建仓库
- [x] 完成现有OV9281驱动适配工作；新系统仓库中的最新修复仍待板级验收
- [x] 调通RK3588的Vulkan运行环境
- [ ] 完成可在真实开发板上验收的系统版本并整理应用交叉编译SDK
- [ ] 在 `apps/edge/` 建立可独立构建、安装和回滚的应用骨架
- [ ] 完成RK3588→MacBook的高速RAW传输与持续丢帧测试
- [ ] 设计帧编号、时钟域、曝光时间和IMU数据协议
- [ ] 开发Mac Metal相位运动放大基线
- [ ] 开发Vulkan局部运动特征、热点扫描和频谱预分析
- [ ] 加入ROI跟踪、NPU目标识别及可选深度估计
- [ ] 集成IMU、自振检测与视觉辅助补偿
- [ ] 完成整机CAD、电子连接、标定和连续运行测试

## 9. 数据、文档与大文件

- **Git：** 源码、配置、Markdown、可编辑设计工程和小型测试样本。
- **按需 Git LFS：** STEP、较大的原生CAD、允许分发的PDF、模型权重；事先确认CNB构建环境的LFS处理方式和存储配额。
- **外部存储：** 连续RAW录像、大型数据集及长时间实验记录；`datasets/` 保留数据索引、采集参数、对应代码/系统版本、标定版本和SHA-256。
- **文档归属：** 整体设计在 `docs/`，具体采购器件信息在 `hardware/modules/`，具体应用构建方法在对应 `apps/*/README.md`。
- **安全：** 不提交设备密钥、SSH私钥、CNB访问令牌、未经授权的厂商资料或包含隐私的原始采集数据。

## 10. 许可证与第三方依赖

本仓库许可证**待确定**。在明确添加 `LICENSE` 前，不应假设这里的源码或CAD工程已被授予特定开源许可。OV9281/RK3588系统仓库、Rockchip SDK、第三方模型及厂商文档分别遵循其自身许可；引入算法代码或预训练权重时，请保存上游仓库、提交版本及许可说明。

---

**VibraReveal · 振见** — *让不可见的振动，被看见。*