# 03-apps/macos · Mac 端应用

在 Apple Silicon MacBook 上运行的接收、分析与可视化应用。

## 职责

- 接收 RK3588 传输的 RAW 帧、时间戳与元数据
- Metal 上的相位运动放大（Riesz Pyramid、Complex Steerable Pyramid 等）
- 局部振动频谱、时频特征与空间振动分布计算
- 交互式选区、热力图与结果导出

## 当前状态

仅目录占位，尚无代码。数据流与模块职责见 [系统架构概览](../../08-docs/architecture/overview.md)。

## 技术栈（规划）

Swift / C++ 混合，计算内核使用 Metal Shading Language。
