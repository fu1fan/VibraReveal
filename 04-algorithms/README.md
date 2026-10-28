# 04-algorithms · 算法参考实现

用于验证与对比的 Python/PyTorch 参考实现，以及模型转换产物。

## 已有实现

- [phase-motion](phase-motion/)：从本机 ELF2 与 MotionAmplification 工程整理的 NumPy/OpenCV 相位运动放大工具，包含离线 CLI、逐帧因果参考、整段 FFT 参考、位移频谱参考和 RAW 抽帧工具。安装、自检、数据格式与限制见该目录 README。
- 当前为 CPU Python 参考实现；不表示 Metal/Vulkan 移植或端到端实时系统已完成。

## 计划内容

- 相位运动放大：Riesz Pyramid、Complex Steerable Pyramid 及可选学习式方法
- 局部振动频谱与时频分析
- 边缘侧预分析算法（Vulkan Compute 版本在 `03-apps/rk3588/` 实现）
- 模型转换：PyTorch → RKNN / CoreML / Metal
