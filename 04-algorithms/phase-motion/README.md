# Python 相位运动放大参考

从本机工程整理，导入日期：2026-10-28。算法源码按原文件复制，来源与 SHA256 见 [sources.json](sources.json)。采用拉普拉斯金字塔、Riesz 局部相位、时域带通、相位旋转及重建；这是现有工程的实用参考实现，不应视为论文算法的完整复现或经过 RTL 等价验证的 golden model。

## 文件与入口

| 文件 | 用途 |
| --- | --- |
| `phasemag.py` | 离线 CLI：视频、RAW、NumPy 栈输入，运动放大、位移测量、频谱和定点量化评估 |
| `algorithm/motion60_reference.py` | `Config` / `Motion60`：逐帧因果 IIR 参考；无效帧、序号间断或尺寸变化会清空历史 |
| `algorithm/motion60_clip.py` | `process_clip_two_pass`：30～120 帧整段 FFT 参考；三层，alpha 0～30；最小 64×64，宽高须为 8 的倍数 |
| `algorithm/motion60_spectrum.py` | `SpectrumConfig` / `MotionSpectrum`：ROI 相位相关位移、去趋势、Hann 窗频谱；无效帧、序号间断或纹理不足会清空窗口 |
| `prepare_raw60.py` | 按整数时间步长抽取 RAW 帧，逐字节复制并输出 JSON 元数据；拒绝覆盖已有文件 |

`motion60` 名称来自原 Zynq 迁移工程，这些模块是可在 PC 导入的 Python 参考，不依赖 FPGA、JTAG 或采集设备。`phasemag.py` 和三个参考模块并非所有浮点细节完全相同，应按所选路径验证结果。

## 安装与自检

建议 Python 3.10 或更新版本；本次实际环境与验证结果见 [VALIDATION.md](VALIDATION.md)。在本目录执行：

```bash
python -m venv .venv
# Windows PowerShell
.venv/Scripts/python -m pip install -r requirements.txt
.venv/Scripts/python -X utf8 phasemag.py --selftest --outdir selftest_out
# Linux / macOS 对应使用 .venv/bin/python
```

以下命令中的 `python` 指已安装依赖的虚拟环境解释器。`matplotlib` 用于绘图，算法可在缺少它时仅输出 CSV。使用 headless OpenCV，不需要桌面 GUI。

## 运行示例

```bash
# 普通视频；不传 --fps 时尝试读取视频标称帧率
python -X utf8 phasemag.py --input speaker.mp4 --lo 5 --hi 12 --alpha 12 --outdir video_out

# OV9281 / rec120 原始 120fps 文件；先用短片段和半分辨率验证
python -X utf8 phasemag.py --input e10hz.raw --fps 120 --scale 0.5 --maxframes 120 --lo 8 --hi 13 --alpha 12 --filt fft --outdir raw_out

# 加入相同机位、参数和采样率的静止对照，测量噪声底线
python -X utf8 phasemag.py --input e10hz.raw --baseline idle.raw --fps 120 --lo 3 --hi 15 --outdir baseline_out

# 120fps 源按隔帧抽取为 60fps；源采样率由调用者确认
python prepare_raw60.py e10hz.raw e10hz_60.raw --source-fps 120 --target-fps 60 --count 120
python -X utf8 phasemag.py --input e10hz_60.raw --fps 60 --maxframes 120 --lo 3 --hi 12 --filt iir --outdir iir_out
```

RAW 文件要求 **1280×800、uint16 小端、低 10 位有效、每行 2560 字节、每帧 2048000 字节**。支持连续 `.raw` / `.bin` 文件，或每文件一帧的目录；目录按文件名排序，帧名应补零。不是 MIPI packed RAW10，也不支持任意 stride。NumPy 输入为 `[T,H,W]` 灰度栈。视频会转灰度；当前视频分支未应用 `--scale`，该参数的降采样行为只适用于 RAW 和 NumPy 输入。

**RAW / NumPy 必须显式传入真实 `--fps`**，否则脚本回退到 30fps；脚本不读取 `.idx` 时间戳或抽帧 JSON，不检测丢帧。输入应预先确认帧完整性、序号连续性和采样率。连续 RAW 尾部不足整帧的字节会被原脚本忽略，应在上游校验文件大小整除 2048000。

输出包括 `mag_sbs.mp4`（左原图、右放大）、`motion.csv`、`report.txt` 和可选 `report.png`。`--no-video` 跳过视频编码；`--roi x,y,w,h` 只改变位移测量区域，坐标对应工作分辨率，不会裁剪放大运算区域。`--maxframes` 默认为 240。

## 参考模块用法

从本目录导入，调用者自行提供 float/整数灰度帧与真实帧序号：

```python
from algorithm.motion60_reference import Config, Motion60
from algorithm.motion60_spectrum import SpectrumConfig, MotionSpectrum

magnifier = Motion60(Config(fps=120.0, lo=3.0, hi=15.0, alpha=12.0))
spectrum = MotionSpectrum(SpectrumConfig(
    fps=120.0, roi=(0, 0, 256, 256), samples=256))
# amplified = magnifier.process(frame, sequence=0, valid=True)
# measurement = spectrum.push(frame, sequence=0, valid=True)
# known_invalid = magnifier.process(frame, sequence=1, valid=False)
```

## 使用边界

- FFT 是整段离线处理；IIR 模块是因果参考，未提供实时采集、传输或显示应用。CPU/GPU 性能和端到端延迟需另测。
- 相位增益 `alpha` 不是已标定的真实位移倍率；alpha=0 仍经过空间分解和平滑重建。
- 整栈加载会占用较多内存；先用 30～120 帧、`--scale 0.5` 的 RAW 短片段验证。
- 要求频带满足 `0 < lo < hi < fps/2`，并保证 ROI 位于图像内。CLI 未完整校验参数。
- 抽帧工具无抗混叠滤波；抽样后新 Nyquist 频率以上的运动会混叠，不能代表原生 60fps 的曝光或传感器时序。
- 固定点选项是 Python 量化误差评估，不证明硬件位精确等价。合成自检也不证明真实场景放大倍率、相机 CRC、原生 RAW8 或链路验收。
- 本次仅导入算法与使用文档，不含本地 RAW、视频、缓存、第三方依赖目录或硬件构建产物。项目许可证仍以仓库根说明为准。
