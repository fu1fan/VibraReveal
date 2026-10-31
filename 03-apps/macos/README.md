# macOS 运动放大后端

Swift Package + Metal 的逐帧相位运动放大基线，macOS 13+，无第三方依赖、无前端。提供库 `VibraRevealBackend` 和命令行 `vibrareveal`。已在 Apple Silicon 上编译、执行 Metal 测试；RK3588 USB 图像接收由 `VibraRevealTransport` 提供，视频容器编解码由调用方适配。

## 两种工作模式

| 行为 | 实时 `realtime` / CLI `stream` | 离线 `offline` |
| --- | --- | --- |
| 分辨率 | 受初始像素/内存预算约束，持续超载时自动降低处理尺寸 | 保留原图或所选 ROI 的原始尺寸，不缩小 |
| 帧率 | 保留输入采样率，不主动抽帧降频 | 任意合法源采样率，处理耗时不受帧周期限制 |
| 输入积压 | 1 帧处理中 + 1 帧待处理；新帧替换尚未处理的旧帧 | 磁盘暂存完整片段，滤波后按原顺序输出全部帧 |
| 资源不足 | 降低空间分辨率；达到下限仍超载则报告 `overloaded` | 明确报错，不自动降低质量 |
| 时间滤波 | 因果二阶 IIR，使用源 FPS | 默认前向—反向零相位带通，可显式选择因果滤波 |

离线模式忽略实时的 `maxWorkingPixels`、`minWorkingPixels`、`maxWorkingBytes`、`maxInputBytes` 和自适应降级策略；仍需满足实际 Metal 设备的内存/单缓冲区能力，以及数据格式的可表示范围。不存在 30/60/120 FPS 档位或 1280×800 尺寸假设。两个模式都要求 `0 < lowHz < highHz < fps/2`，并拒绝 Float32 下不稳定的极窄/极低频带；这属于采样和数值有效性校验。

## 构建与测试

```bash
cd 03-apps/macos
swift build -c release
swift test
swift run -c release vibrareveal --help
```

如果工作目录附带 Finder 扩展属性导致 XCTest 签名报 `resource fork, Finder information ... not allowed`，将构建产物放到临时目录：

```bash
swift test --scratch-path /tmp/vibrareveal-macos-build
swift build -c release --scratch-path /tmp/vibrareveal-macos-release
swift build -c release --scratch-path /tmp/vibrareveal-macos-release --show-bin-path
# 将上一条输出路径下的 vibrareveal 传给集成检查（从本目录执行）
python3 ../../06-tests/macos_backend_cli.py /path/to/built/vibrareveal
```

Metal 源码作为包资源随库分发，在初始化时编译一次；无需单独手工编译 metallib。初始化应在采集开始前完成。

## Swift 接入

```swift
import Foundation
import VibraRevealBackend

let config = Configuration(
    fps: 119.88, lowHz: 5, highHz: 15, amplification: 10,
    roi: Region(x: 100, y: 80, width: 513, height: 377),
    mode: .realtime
)
let backend = try RealtimeMagnifier(configuration: config) { result in
    switch result {
    case .success(let frame):
        // 消费 frame.pixels；读取实际 width/height、historyReset、performance。
        // 回调运行在处理队列；应及时返回，避免慢消费者引起输入替换。
        print(frame.sequence, frame.width, frame.height, frame.performance.status)
    case .failure(let error):
        print(error)
    }
}
// 相机/网络回调中提交，每帧提供真实采集序号、单调采集时间（秒）：
// backend.submit(InputFrame(data: bytes, width: w, height: h, bytesPerRow: stride,
//     format: .gray16LE(validBits: 10), sequence: seq, timestamp: captureSeconds))
// 停止生产后：
backend.close()
backend.waitUntilIdle() // 不可在结果回调中调用
```

离线采用完整片段入口，逐帧读入磁盘暂存，完成零相位滤波后逐帧输出；不把整段视频放入内存：

```swift
let config = Configuration(fps: 240, lowHz: 8, highHz: 20,
                           amplification: 200, mode: .offline)
let processor = try OfflineMagnifier(configuration: config)
// let report = try processor.process(
//     spectrumProbe: SpectrumProbe(level: 0, x: 100, y: 80),
//     readNext: { try source.readNextFrame() },
//     emit: { try sink.write($0) }
// )
// report.spectra?.input / .filtered 提供局部相位的原始/滤波后频谱。

```

`OfflineMagnifier` 和 `MetalMagnifier` 均为串行对象，不支持重入。`MetalMagnifier.process` 仅接受因果配置；零相位必须使用 `OfflineMagnifier`，不能逐帧获得因果式即时结果。`MetalMagnifier` 本身为串行对象，调用方不得并发调用 `process/configure/reset`。`RealtimeMagnifier.submit` 支持并发生产者，但调用方仍应保证提交的帧按采集顺序到达。离线配置不能传给实时队列，以免误用导致丢帧。

`configure` 校验成功才更换配置，并清空时域历史。更换帧率/ROI/频带时，同步入口可调用 `configure`；异步入口应关闭并排空旧实例，再创建新实例。分辨率、位深变化会自动检测。`reset` 可用于跳转、重新连接或时间基准改变。

## 输入和输出约定

- 灰度 `.gray8`，或 `.gray16LE(validBits: 1...16)`；10/12 位值需在 16 位小端容器中低位对齐。每行可带 padding。MIPI packed RAW10/12、Bayer、RGB、CVPixelBuffer 需在适配层转换。
- 输入宽高可以是奇数、单像素轴，不要求整除 2/8/线程组大小。当前表示范围为每轴 1...65535，单帧字节数不超过 UInt32 上限和设备缓冲区能力；实时默认输入限额为 256 MiB。
- `data.count == bytesPerRow * height`，包含最后一行 padding；空帧、截断、非法 stride、位深及非有限时间戳会报错。
- `timestamp` 使用同一单调**采集时钟**的秒数。不能用网络到达时间代替；FPS 可以是 59.94 等非整数。
- 输出是所选 ROI 的归一化 Float32 灰度平面，不拼回原图。相位放大可能产生小于 0 或大于 1 的值，编码/显示时由调用方决定裁剪。
- 结果包含原始尺寸、ROI、实际处理尺寸、原始序号、采集时间、模式、实际 `temporalFilter`、`historyReset` 及性能统计。
- 对逐帧因果入口：序号断档/重复/倒退、采集时间间隔偏离 `1/fps` 超过 20%、空间尺寸/位深/配置改变都会重置历史。重置帧不添加放大信号；后续 IIR 需要重新稳定。当前不是可变时间步滤波器；真实可变帧率视频需先做时间重采样，或按稳定采样率片段分别处理。
- 完整片段离线入口要求宽高/stride/位深固定、序号连续、采集间隔偏差不超过 1%；不满足时在输出前报错。零相位首帧也利用未来样本，`historyReset` 不表示该帧没有放大信号。

## 实时性能约束

默认初始上限 1,048,576 工作像素、256 MiB 金字塔缓冲区、最低目标 256 像素。尺寸按 ROI 比例缩小并取整；极细 ROI 也会严格遵守像素上限。内存预算可以进一步降低尺寸，实际结果以输出元数据为准。预算不包含生产方持有的数据、输出副本或接收端自行缓存的结果。

每帧测量上传、Metal 执行、同步等待和 CPU 结果拷贝的总耗时；预算为 `utilization / fps`（默认 0.75 个源帧周期）。前 3 帧作为启动观察期，随后使用 EMA；连续 3 次超预算后将目标工作像素减半。分辨率改变会清空历史。为防止来回震荡，当前会话只降不升，重新配置后可重新探测。

`performance.status` 为 `warmingUp / sustainable / reducingResolution / overloaded`，离线为 `offline`。`recommendedMaxFPS` 只是基于当前处理耗时估算的吞吐建议，**不是相机控制指令，也不保证链路吞吐**。离线的 `budgetSeconds` 为 0，表示不限制处理时长。

达到最低工作尺寸仍超载时，调用方应缩小 ROI 或重新协商真实相机帧率，同时重新校验 Nyquist；后端不会通过静默抽帧伪装成较低采样率。输入不可阻塞时有界队列仍可能替换待处理帧，`Submission.replaced` 和 `statistics.dropped` 可见；后续结果会因序号缺口重置历史，不能把这些间断结果当连续振动测量。

慢输出消费者同样会导致队列替换，但其耗时不计入计算吞吐估计。采集、传输、编解码和持续热负载必须另做端到端验收。

## 命令行（无 GUI）

```bash
# 本机合成纹理测量；JSON 报告输出到 stdout，设备信息到 stderr
vibrareveal benchmark --width 1280 --height 800 --fps 120 --frames 180
vibrareveal benchmark --mode offline --width 321 --height 241 --fps 120 --frames 120

# 实时：上游持续写入原始帧，后端读取后立即处理
camera-adapter | vibrareveal stream --width 1280 --height 800 --fps 120 \
  --bits 10 --low 5 --high 15 > realtime.vrm

# 离线：完整处理每帧，输出保持 1921×1081，即使速度低于源 FPS
vibrareveal offline --width 1921 --height 1081 --fps 240 \
  --bits 16 --low 8 --high 20 --alpha 200 --filter zero-phase \
  --spectrum spectrum.json --probe 100,80 < recording.raw > amplified.vrm

# 离线 ROI：保持选区原生像素，不受实时 max-pixels 限制
vibrareveal offline --width 1280 --height 800 --fps 120 --bits 10 \
  --roi 100,80,513,377 --low 5 --high 15 < recording.raw > roi.vrm
```

stdin 是固定宽高、位深和 stride 的连续无头原始帧；CLI 以 `sequence / fps` 合成时间戳，不读取相机头或 idx 文件。要保留真实相机时间、原始序号或动态尺寸，请用库 API。不要用 `stream < file.raw` 验证完整离线输出：文件高速灌入会触发实时队列替换，应使用 `offline`。

stdout 的每帧布局：`UInt32 小端 JSON 字节数` + `UTF-8 JSON` + `width × height 个小端 Float32`。JSON 包含 `version=1`、`pixelFormat=float32le` 和上述输出元数据，帧与帧直接拼接。`.vrm` 是此工具的简单输出后缀，不是 MP4 或已定稿的 RK3588 网络协议。日志写 stderr，不混入数据流。CLI 拒绝 EOF 处的残缺帧。

## 算法与验证边界

沿用 Python 因果参考的实用方案：高斯/拉普拉斯金字塔、5×5 可分离 Riesz 近似、局部相位展开、因果二阶带通、相位旋转和逐层重建。Metal 处理任意实际尺寸；实时降采样先做空间 box 积分，避免直接抽取像素。

与 `motion60_reference.py` 不做位精确对齐：拉普拉斯层改为 `当前层 - 上采样(下一层)`，保留可精确重建的细节，使 `alpha=0` 在不缩放时近似恒等（Float32 误差）；小层最低为 1 像素，尺寸无需补齐。相位增益不是标定后的物理位移倍率。实时采用因果 IIR；离线默认前向—反向零相位滤波，空间分解与重建共用相同的 Metal 内核。

自动测试覆盖尺寸、ROI/RAW stride、重建、静止/黑图、时序缺口、空间抗混叠、负载控制、离线完整性及带内/带外合成运动响应；具体执行记录见 [VALIDATION.md](VALIDATION.md)。尚未完成真实全局快门相机、网络吞吐、长时热稳定、真实场景放大倍率和画质的验收。

## 可调滤波器与 200 增益

`Configuration.temporalFilter` / CLI `--filter auto|causal|zero-phase`：

- `auto`：实时选择因果，离线选择零相位。
- `causal`：单向二阶带通，可用于实时或离线对照；`MetalMagnifier.process` 提供逐帧入口。
- `zero-phase`：离线前向—反向应用带通滤波，等效幅频响应为单向响应的平方、内部稳态没有时间相位延迟；不支持实时模式。`lowHz/highHz` 仍是带通设计参数，前后向响应比单向更窄，不声称这些值是双向滤波严格的 −3 dB 截止点。

离线实现对每个金字塔像素的局部相位做时间滤波，使用 Double 递推状态、Float32 空间相位存储；先进行端点奇对称延拓，再前向、反向处理，最后裁去延拓区。每端延拓 `min(N-1, ceil(3*fps/lowHz))` 帧，实际值在 `OfflineReport.paddingFramesPerEnd` 中给出。至少需要 3 帧；短于数个低频周期的片段仍可能有明显端点效应，不能通过延拓创造不存在的测量信息。

`amplification` / `--alpha` 统一允许 **0～200**。它是相位旋转增益，不保证物理位移恰好放大 200 倍；大增益可能产生局部相位绕回及画面失真。输出不做隐式增益限幅或灰度裁剪。

## 频域输出

离线命令使用 `--spectrum report.json` 保存完整报告。默认选择 ROI 第一层中心，也可用 `--probe x,y --spectrum-level L` 指定。坐标相对该 ROI 金字塔层，不是全图坐标；层号从 0 开始，各层尺寸按当前金字塔的向下取整规则缩小。

报告记录源尺寸、ROI、采样率、频带、增益、起始序号和采集时间。同时提供所选位置的**原始局部相位**与**时间滤波后、乘增益前的局部相位**频谱，避免以整幅图像亮度均值代替运动信号：

- `frequencyHz`：单边频率轴，覆盖 DC 至 Nyquist。
- `amplitude`：相干增益校正后的幅度（rad）。
- `powerSpectralDensity`：按窗能量归一化的 PSD（rad²/Hz）。
- `peakFrequencyHz`：DC 之外最大的幅度峰；零信号时省略此字段。它是频率栅格上的峰，不是亚频点估计。
- `sampleRate/sampleCount/fftSize/binSpacingHz/observationResolutionHz`：采样率、观测长度与频率分辨率；补零仅加密栅格，不提高真实分辨率。

频谱至少需要 4 帧，采用去均值、周期 Hann 窗、补零到下一 2 的幂的 FFT。单边幅度及 PSD 对非 DC/Nyquist 项加倍；未做去线性趋势。无纹理位置可能没有有效运动信号，应选择有纹理且与目标运动相关的位置。这里输出的是相位代理频谱，不是已标定的位移/速度/加速度，也不是整图频率热力图。

库提供独立 `SpectrumAnalyzer.analyze(samples:fps:unit:)`，可分析调用方收集的连续等间隔信号；实时窗口应由调用方处理序号缺口与窗口长度，不能把丢帧窗口当作连续采样。离线 `OfflineMagnifier.process(spectrumProbe:readNext:emit:)` 自动提供相位信号及频谱。

## 离线临时存储和耗时

默认临时目录位于系统临时文件夹，可通过 `--scratch-dir PATH` 或 API 的 `scratchDirectory` 放到足够空间的磁盘。使用独立 UUID 子目录，成功或抛错均清理；磁盘写入失败直接报错，不降分辨率、不丢帧。

设原始每帧 B 字节、所有相位层共 P 字节、N 帧、每端延拓 K 帧，零相位临时文件峰值约 `N*B + (3*N + 2*K)*P`。RAM 不随完整图像栈增长，但需容纳若干空间帧、每像素滤波状态和 O(N) 的时间元数据/可选探针信号。

`OfflineReport.processingSeconds` 记录完整任务耗时（读取、暂存、两遍滤波、重建、输出及可选频谱）；每个 `OutputFrame.performance` 只计最后重建阶段，不可据此推算离线整体吞吐。离线 benchmark 同样报告完整任务时间，包含合成数据生成。

## RK3588 USB 输入

图像走 USB NCM，SSH 和文件部署走 Wi-Fi。使用方式、CNB 编译与线路配置见
[RK3588 发送端](../rk3588/README.md)，字节格式见 [VRI1](../../05-protocol/README.md)。

```sh
# 独立接收，尽可能释放 USB/磁盘吞吐；输出保留逐帧采集元数据
vr-receive --host 192.168.77.1 --output capture.vri
# 协议校验和吞吐测试，不写盘
vr-receive --host 192.168.77.1
# 按 sender HELLO 使用真实 FPS、尺寸、位深和时间，不能另传 --fps/--width 等覆盖
vibrareveal stream --connect 192.168.77.1 --low 3 --high 12 > amplified.vrm
vibrareveal offline --archive capture.vri --low 3 --high 12 > amplified.vrm
```

网络实时输入必须匹配实时发送模式，网络离线输入必须匹配离线发送模式。
录像可送入任一算法入口，但离线算法仍要求固定尺寸、连续序号和等间隔采样；
带丢帧/尺寸变化的实时录像不会被静默当作完整离线片段。实时 ROI 坐标相对传输图像，
链路自适应缩小时调用方需相应调整；默认全帧处理不需要 ROI 坐标重映射。

库的 `FrameReader.readNext()` 直接返回后端 `InputFrame`，使用采集时间而非到达时间。
`info` 保存源尺寸/FPS/时间来源标志，`statistics` 保存接收帧数、有效字节数、序号缺口和发送队列丢弃数。
FrameReader 是串行对象；实时数据读取应持续提交给 `RealtimeMagnifier`，避免处理阻塞网络读取。
