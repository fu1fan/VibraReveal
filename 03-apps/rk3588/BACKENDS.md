# RK3588 五项计算后端

本接口只提供算法和命令行，不含 UI、HDMI、自动调焦电机或视频编解码。
实时图像采集/传输仍由现有采集服务负责；新库可接收该服务复制出的 `Frame`。
不要同时启动两个程序争抢同一 V4L2 节点。

| 工作 | 设备 | 接口 / 程序 | 输出 |
| --- | --- | --- | --- |
| 峰值对焦辅助 | Vulkan GPU | `VulkanMotion::analyze` / `vr-features` | 峰值像素掩膜、归一化梯度、平均梯度平方分数 |
| 交互物品选择 | RKNN NPU | `MobileSam::setImage/select` / `vr-select` | 原图尺寸物体掩膜、模型置信分数、帧身份 |
| 物品边缘及邻域 | Vulkan GPU | `VulkanMotion::analyze` / `vr-features --mask` | 选中边缘、边缘两侧测量带、法线方向 |
| ROI/降帧率实时放大 | Vulkan GPU | `VulkanMotion::process` / `vr-motion` | VRV1 float32 帧，严格实时预算 |
| 全视频非实时放大 | Vulkan GPU | `vr-offline` | 完整 VRV1 文件，保持每帧序号、时间及源 FPS |

## 构建

```sh
elf2-cmake -S 03-apps/rk3588 -B /tmp/vr-backend-build \
  -DCMAKE_BUILD_TYPE=Release -DVR_BUILD_VULKAN_MOTION=ON \
  -DVR_BUILD_RKNN_SELECTION=ON -DRKNN_INCLUDE_DIR=/path/to/rknn/include
elf2-sdk run cmake --build /tmp/vr-backend-build --parallel 4
```

需要宿主 `glslangValidator`，SDK Vulkan headers/loader，以及 Rockchip 官方 `rknn_api.h`。
NPU 运行时通过 `dlopen("librknnrt.so")` 加载，不把构建宿主的库误链接进 ARM64 程序。
只有运行 `vr-select` 或实例化 `MobileSam` 才需要 NPU/运行时/模型。
关闭两个构建选项仍可单独构建传输程序。

## 1. 对焦与边缘

```sh
vr-features --input recording.vri --output-dir focus-results \
  --shader motion.comp.spv --threshold 0.04 --every 8
vr-features --input recording.vri --output-dir selected-edge-results \
  --shader motion.comp.spv --mask object.pgm \
  --threshold 0.04 --boundary-radius 2 --region-radius 4
```

`--every` 仅抽取预览分析帧，输入仍完整读取/验证。此策略不可用于运动时域滤波。
输出目录必须不存在。每个分析帧输出 `<sequence>-peaks.pgm`、`-edges.pgm`、`-regions.pgm`，
P5 灰度 0/255、原始分辨率。`frames.jsonl` 含帧序号、单调时间 ns 和 `focus_score`；
该文件完整发布才表示整个任务成功，失败可能留下部分 PGM 和 `.partial` 元数据。

GPU 流程为 5-tap Gaussian → Sobel → 梯度法线方向非极大值抑制 → 阈值 → 掩膜范围筛选 → 邻域扩张。
这是确定性强边缘筛选，不宣称实现了完整 Canny 双阈值滞后连接。
强度始终来自原始图像；物体掩膜只限制候选边缘，不能制造物体轮廓。
`boundary-radius` 允许搜索掩膜外少量像素以容忍分割误差；`region-radius` 保留边缘两侧像素。
空掩膜参数表示全图，实际全零掩膜表示不选任何物体，二者不同。

实时调用时设置 `MotionConfig.analysisOnly=true`，按需调用 `analyze(frame, edgeConfig, mask)`。
结果包含 `magnitude/normalX/normalY/peaks/edges/regions` 和源帧身份；函数不修改输入。
实例固定尺寸且不可并发调用。由调用端在独立预览工作线程传入最新帧，允许丢弃过时预览任务，
不能在采集线程里同步等待 GPU。分析有 30 秒故障等待上限，不承诺全分辨率 120 FPS。

`focusScore` 是掩膜内部平均梯度平方，适合固定场景、固定曝光时比较对焦清晰度；
不是焦距或物理位移。曝光变化/纹理变化也会改变该分数。
边缘法线对该方向运动更敏感；方向未知时应选择多个朝向，或纹理/角点区域。
本版不做跨帧稳定性排序和自动追踪。测振开始前固定 ROI；选区改变后新建放大实例。

## 2. MobileSAM 物品选择

输入为任意类别交互分割，不依赖固定类别检测头；困难目标仍需要用户修正。
模型输入是原始灰度线性缩放到 0..255 后复制到 RGB 三通道，不对测量原图做增强。
长边 resize 到 448，右/下补零；均值/标准差由 RKNN 转换固化。
掩膜按 SAM 的两级双线性插值、去 padding 流程还原到原图。

```sh
# prompts.txt 每行：原图 x y label。下面两行表示一个框。
printf '190 70 2\n460 280 3\n' > prompts.txt
vr-select --input recording.vri --frame-index 0 \
  --encoder mobilesam_encoder.rknn --decoder mobilesam_decoder.rknn \
  --prompts prompts.txt --output object.pgm
```

`label=1` 为正点，`0` 为负点，`2/3` 为左上/右下框角；一个框最多一对。
坐标须位于原图范围内；点提示必须至少有一个正点。纯点输入保留一个 `-1` padding slot。
提示数量依据模型 tensor 查询，超限明确报错，不截断。本仓库转换器默认 8 slots，可同时输入框和正负点；
纯点提示最多 7 个。官方原示例为 2 slots，仍可加载但容量相应受限。
当前模型适配器支持 2..64 slots 的相同五输入布局；不自动改写模型图。

程序验证完整录像后选择零起始的 `frame-index`，输出 P5 掩膜和同名 `.json` 元数据。
`MobileSam::setImage` 缓存该帧 embedding；同一实例可多次 `select`，便于修改提示。
设置新图像后旧 embedding 失效。掩膜带帧身份，调用者必须决定如何应用到后续帧，
不能把过时选区当作已完成追踪。模型 score 是预测质量分数，不是实测 IoU。

模型转换及来源见 [models/README.md](models/README.md)。灰度域选区质量仍需覆盖实际器件验证。

## 3. 实时与离线运动放大

实时用法、整数抽帧约束和截止时间错误仍见 [README](README.md#板端-vulkan-roi-实时运动放大)。
新特征/NPU任务没有被直接塞进现有实时线程，避免未经负载验收便声称同时运行达标。

```sh
# 默认整幅原始图像、全部帧；只受设备资源约束，不因处理慢而丢帧。
vr-offline --input recording.vri --output amplified.vrv \
  --shader motion.comp.spv --low 3 --high 12 --alpha 12 --levels 3 \
  --filter zero-phase --scratch-dir /path/to/scratch
# 显式选择 ROI 时仍保留每个时间采样点。
vr-offline --input recording.vri --output roi.vrv \
  --shader motion.comp.spv --x 200 --y 100 --roi-width 256 --roi-height 256 \
  --low 3 --high 12 --alpha 12
```

离线参考 macOS `OfflineMagnifier`，分三阶段：GPU 提取完整片段的各层局部相位，
CPU Double 状态做时间带通，GPU 将滤波后的相位旋转并重建。相位/中间结果存 Float32。
`--filter auto|zero-phase|causal`：默认 `auto` 在离线选择 `zero-phase`；`causal` 显式选择单向滤波。
实时 `vr-motion` 仍只使用原有逐帧因果算法，不能通过逐帧接口取得零相位结果。

零相位采用端点奇对称延拓、前向/反向二阶 IIR，最后裁去两端延拓区。
每端 `K=min(N-1, ceil(3*fps/low))` 帧；至少需要3帧。内部稳态无时间相位延迟，
等效幅频响应是单向响应的平方；`low/high` 是设计参数，不是双向滤波严格的 −3dB 截止点。
短片段仍有端点效应，延拓不能替代真实采样。首帧也利用未来样本。

`--scratch-dir` 指定已存在的暂存目录（默认 `/tmp`）；任务在其下创建私有子目录，
正常返回或抛异常时清理；进程被强制终止时可能留下暂存。原始帧、相位、前向结果、
最终相位及16字节/帧的序号和时间全部存磁盘，RAM不随视频时长增长。
原始每帧B字节、相位每帧P字节，零相位暂存峰值约 `N*B+(3*N+2*K)*P+16*N`；
因果模式约 `N*B+2*N*P+16*N`。`--max-scratch-bytes` 默认64GiB，超限或磁盘写入失败明确报错。
长视频应使用容量足够的磁盘目录；板上 `/tmp` 可能是RAM盘，不能把它当无限容量存储。

完整输入及时间滤波通过后才输出第一帧。不降低空间分辨率、不抽帧、不按墙钟限速。
最大区域仍为1,048,576像素，覆盖OV9281 1280×800全帧，另受Vulkan storage buffer限制。
不支持的尺寸报错，不暗中缩小。GPU硬件挂死仍可能导致驱动等待。
完成日志包含 `filter`、`padding_frames_per_end`、`scratch_bytes`、`frames` 和总处理时间。
库入口为 `processOffline(config, shader, options, readNext, emit)`，详见 `offline_motion.hpp`。

输入支持本项目固定格式、原始分辨率的 VRI1 灰度录像（8/10/12/16-bit，允许行 padding）。
拒绝 CRC 错误、丢帧计数、帧序号间断、超过 ±1% 的采样间隔偏差、格式切换、缺失 END 或尾随数据。
视频容器先在外部解码并保留时间信息，再封装 VRI1；本后端不提供 MP4 编解码。
输出是现有 VRV1；源合成时间标志在完成日志中明确记录。输出在通过 END 校验后 fsync、
以不覆盖目标文件的方式发布；失败留下 `.partial`，不能把它当完整结果。

## 验证

```sh
ctest --test-dir /tmp/vr-backend-build --output-on-failure
python3 06-tests/vulkan_motion.py /tmp/vr-backend-build/motion-test.f32
python3 06-tests/vulkan_motion_cli.py /tmp/vr-backend-build/vr-motion /tmp/vr-backend-build/motion.comp.spv
python3 06-tests/rk3588_backend.py /tmp/vr-backend-build
```

数值测试、实际 Mali/NPU 运行、相机实时并发和真实振动测量精度是不同验证范围。
整段滤波对照结果见 [零相位验收](../../08-docs/validation/rk3588-offline-filter-2026-10-31.md)；
此前五项后端验收见 `08-docs/validation/rk3588-backends-2026-10-31.md`。
