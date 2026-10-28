# 导入验证记录

日期：2026-10-28。环境：Windows x64，独立 venv，Python 3.13.15、NumPy 2.5.4、opencv-python-headless 5.0.0.93（`cv2.__version__ = 5.0.0`）、matplotlib 3.11.2。完整依赖版本见 [requirements-tested.txt](requirements-tested.txt)。`requirements.txt` 保留原工程的未锁版本依赖；需要复现本次环境时可安装 `requirements-tested.txt`。

## 原脚本合成自检

```bash
python -X utf8 phasemag.py --selftest --outdir selftest_out
```

退出码 0，5 项检查全部 PASS：

| 检查 | 实测 |
| --- | --- |
| x 主频 | 8.00 Hz |
| y 主频 | 5.00 Hz |
| x 带内位移 RMS | 0.265 px |
| y 带内位移 RMS | 0.158 px |
| x 信噪余量 | 22.5 dB；静止噪声底线 0.0199 px |

生成的 MP4 可由 OpenCV 重新打开并解码，帧数为 180。

## 导入时补充检查

在同一环境通过一次性合成数据检查（不含真实采集数据）：

- 六个复制文件均与本机来源逐字节一致，SHA256 已写入 `sources.json`。
- 60 帧、64×64、60fps、x 方向 0.3px / 8Hz Fourier shift 合成序列：三层、3～12Hz、alpha=12 的 `process_clip_two_pass` 对比原 `build_bands → process_band(fft) → synthesize`，最大绝对差 `9.1552734375e-05 DN`，通过 `rtol=1e-5, atol=0.002` 比较。
- 同长度随机相位张量：`filter_phase_tile` 与原 `temporal_bandpass(fft)` 通过 `rtol=1e-5, atol=2e-6` 比较。
- `Motion60` 对全部合成帧输出有限值；帧序号跳变后的输出与新实例首帧输出逐元素相同；无效帧返回 `None` 并清空序号。
- `MotionSpectrum` 在 60 帧窗口中检出 x 方向 8Hz；序号跳变后窗口重新从 1 帧开始；无效帧返回无效结果。
- `prepare_raw60.extract` 对四帧完整 RAW 按 120→60fps 抽取两帧，输出逐字节等于源第 0、2 帧；再次写入同名输出被拒绝。
- `git diff --check` 无空白错误。

这些检查不构成硬件验收、实际场景性能测试、固定点全覆盖验证或实时系统验收；本次没有运行 `--fx` 自检。合成位移测试不直接测量放大后位移倍率。
