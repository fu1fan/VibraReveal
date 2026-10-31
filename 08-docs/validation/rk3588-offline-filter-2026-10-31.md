# RK3588 整段零相位滤波验收（2026-10-31）

替代首轮 `vr-offline` 仅有因果处理的限制。参考本仓库 macOS
`OfflineMagnifier.swift`、`PhaseIIR` 和 `Motion.metal`，没有修改 macOS 算法。

## 行为

- 默认 `--filter auto` 解析为 `zero-phase`；可显式选择 `causal`。
- GPU 提取各层局部相位；CPU Double 递推、Float32系数/中间文件；GPU按外部相位旋转并重建。
- 每端奇对称延拓 `min(N-1, ceil(3*fps/low))`，前后向带通后裁剪；零相位至少3帧。
- 整段通过CRC/END、连续序号、固定格式及1%时间间隔检查后，再滤波和输出。
- RAW、相位、中间结果和时间元数据写私有暂存目录。正常完成及异常返回均清理；支持暂存字节预算。
- 保留原始分辨率、每帧序号/时间/FPS。实时逐帧接口继续使用因果滤波。

## 代码与独立数值测试

CNB `cnb-uj9-1k68t3brn`，固定SDK digest仍为
`d2a21493198e25881cb03144bf4dd45b76e773e6cbbad1b62d6debbd4aa73cef`。
GNU aarch64 10.3.1交叉编译；Linux llvmpipe运行Vulkan验证层/同步验证。

- CTest三组通过：原运动算法、峰值/边缘、整段离线。
- 旧数值/录像完整性回归通过；因果测试显式传 `--filter causal`。
- 新测试覆盖alpha=0、1帧因果、少于3帧零相位拒绝、空片段、序号缺口、3%时间偏差拒绝、
  输出回调抛错、暂存预算不足、所有成功/失败路径的暂存清理。
- 480帧、21×17、16-bit且含行padding的10Hz移动纹理，源FPS120，频带5–15Hz，alpha20。
  独立NumPy实现空间分解、Double时间递推、奇对称延拓与重建；每端延拓72帧。
- llvmpipe与NumPy最大绝对误差 `1.2905953677977422e-6`。

复现：

```sh
python3 06-tests/offline_filter.py prepare /tmp/fixture
vr-offline --input /tmp/fixture/moving.vri --output /tmp/fixture/result.vrv \
  --shader motion.comp.spv --low 5 --high 15 --alpha 20
python3 06-tests/offline_filter.py check /tmp/fixture /tmp/fixture/result.vrv
```

## Native Metal 与 Mali 对照

本机从当前macOS源码构建release程序，在Apple M5上实际执行同一VRI1文件：

```sh
vibrareveal offline --archive /tmp/fixture/moving.vri \
  --low 5 --high 15 --alpha 20 --filter zero-phase > /tmp/fixture/metal.vrm
python3 06-tests/offline_filter.py compare /tmp/fixture /tmp/fixture/result.vrv /tmp/fixture/metal.vrm
```

| 比较 | 最大绝对灰度误差（标称0..1） |
| --- | ---: |
| Metal vs NumPy | 1.7405480138288354e-6 |
| llvmpipe Vulkan vs Metal | 1.3709068298339844e-6 |
| Mali Vulkan vs NumPy | 2.486992374595154e-6 |
| **Mali Vulkan vs Metal** | **2.384185791015625e-6** |

逐帧验证480帧的输出尺寸、序号、时间和FPS；内部120..359帧探针的正交/同相分量比小于0.03，
放大信号能量大于1e-6，验证非零放大及无明显时间相位延迟。
此夹具在正弦零交叉开始，首帧结果可以恰好接近原图；不以首帧是否有变化判断是否使用未来样本。

板端Mali-G610：
- C++整段离线测试通过。
- 480帧小片段：暂存3,278,592字节，处理约0.814秒。
- 完整性合格的1280×800 Bulk录像截取16帧，频带3–12Hz，alpha12：输出16帧，
  每端延拓15帧，暂存452,096,256字节，处理约3.122秒。
- 两轮结束后指定暂存父目录为空。没有重启或替换采集服务。
- 原Wi-Fi SSH控制会话失效，本轮新程序和测试输入通过已有ADB调试连接传送/执行。

计时为单次整体处理耗时，含暂存与初始化，不是持续吞吐/P95承诺。
双向响应为单向幅频响应的平方；短片段仍有端点效应，不等于物理位移标定或无限长数据的理想滤波。

## 交付包

`vr-rk3588-backends-20261031-zero-phase.tar.gz` 更新GPU程序、shader、静态库及新增 `offline_motion.hpp`；
保留前一版NPU模型和隔离runtime。包内 `offline-build-info.json` 记录本次源码哈希，
对其中列出的文件替代首轮 `build-info.json` 的对应身份。
板端解压后全部文件SHA256通过；`bin/offline` 包装器不显式指定filter即采用zero-phase，480帧冒烟通过。
包SHA256与数值结果见 [JSON记录](rk3588-offline-filter-2026-10-31.json)。
