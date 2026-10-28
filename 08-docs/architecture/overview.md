# 系统架构概览

> 本文描述 VibraReveal 的目标架构。

## 1. 数据流

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

## 2. 软件模块与计算职责

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
