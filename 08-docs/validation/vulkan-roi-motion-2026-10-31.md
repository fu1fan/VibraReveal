# RK3588 Vulkan ROI 运动放大验收（2026-10-31）

实现与使用方法见 [板端 README](../../03-apps/rk3588/README.md#板端-vulkan-roi-实时运动放大)。
此记录验证 Vulkan 计算、相机输入、短时实时预算和超载报错；不包含 HDMI 显示、视频编码、
真实位移倍率标定或长时间热稳定性验收。

## 构建与安装

- CNB 云端开发环境 `cnb-dq3-1k68p5981`，GNU aarch64 10.3.1，Release。
- 固定 SDK `d2a21493198e25881cb03144bf4dd45b76e773e6cbbad1b62d6debbd4aa73cef`，系统提交 `ac7c05413b3882f578cb94be4b2edfe3958d84c6`。
- 源文件从本地工作区上传 CNB 后构建，没有把 Mac 产物作为板端程序；本次代码尚未提交，
  [JSON 记录](vulkan-roi-motion-2026-10-31.json) 保存基线提交、逐源文件与二进制 SHA256。
- 板端 GPU 为 Mali-G610，驱动报告 `g24p0-00eac0` / `rk_so_ver: 10`。
- 已安装 `/usr/local/bin/vr-motion`、`/usr/local/share/vibrareveal/motion.comp.spv`；
  `/usr/local/share/vibrareveal/motion-build-info.json` 保存构建身份。默认 shader 路径安装冒烟通过。
- 相机测试期间暂停 `S91vrstream`，结束后恢复并确认 `vr-stream` 运行。未修改 sensor 帧率/vblank。

## 数值、边界和输出验证

- Linux llvmpipe：C++ 单元测试通过，启用 Khronos validation layer 与同步验证；SPIR-V Vulkan 1.0 校验通过。
- 同一 C++ 测试在 Mali-G610 通过，包括三层、64 帧非零相位增益、31×23 奇数 ROI、带行 padding 的 Y10，
  alpha=0 重建、1 像素宽/高区域、错误参数/损坏帧、序号和时间戳缺口、格式变化、失败会话拒绝继续。
- 独立 NumPy 参考：llvmpipe 最大绝对误差 `3.2492609e-6`；Mali 为 `5.1674332e-6`（灰度标称 0～1）。
  该数值一致性不等于真实物理位移倍率已标定。
- CLI：参数错误、VRV1 帧头与 payload、三帧 stdout、输出管道不读取时的 `OUTPUT_DEADLINE_EXCEEDED` 均通过。
- 板端真实相机 stdout：256×256@30、30 帧共 7,865,520 字节；逐帧校验头、长度、原始序号间隔 4，
  时间间隔在目标 FPS 的 ±20% 内。无 Mali 驱动日志混入二进制。

## 真实相机短测

OV9281 全尺寸 1280×800、Y10 非 compact，sensor 查询为 120 FPS。
处理区域没有降采样，三层、low=3 Hz、high=12 Hz、alpha=12；不指定输出时仍执行 GPU 读回。

| ROI | 目标 FPS | 结果 |
| --- | --- | --- |
| 256×256，位置 (512,272) | 30 | 300 帧完成，约 10 秒 |
| 128×128，位置 (0,0) | 120 | 360 帧完成，约 3 秒 |
| 256×256，位置 (0,0) | 60 | 180 帧完成，约 3 秒 |
| 256×256，位置 (0,0) | 120 | `PROCESSING_DEADLINE_EXCEEDED`，处理 9.598 ms > 8.333 ms |
| 512×512，位置 (0,0) | 120 | `GPU_DEADLINE_EXCEEDED`，fence 等待 8.777 ms 超预算 |
| 1280×800 | 120 | `GPU_DEADLINE_EXCEEDED`，fence 等待 8.549 ms 超预算 |

上述成功项没有报告采集序号缺口、队列溢出或超预算；失败项均返回退出码 2，未静默降低尺寸或 FPS。
GPU 超时数据是截止时的等待时长，**不是 GPU 完整任务耗时**，也不能排除驱动调度影响。
本轮包含冷启动及运行中的严格每帧检查，没有通过 warm-up 豁免超时。
这些短测不是最大可用配置承诺；负载、温度、输出端和驱动调度可能使同样参数失败。
