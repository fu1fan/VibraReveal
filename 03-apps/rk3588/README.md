# RK3588 图像采集与计算后端

五项 GPU/NPU 后端、接口及命令见 [BACKENDS.md](BACKENDS.md)。

`vr-stream` 是 C++17 单会话发送器，支持 V4L2 相机、已有 RAW 文件及合成负载。
图像走自定义 USB Bulk；管理和部署走 Wi-Fi。协议与队列策略见 [VRI1](../../05-protocol/README.md)。

## CNB 构建

在已启动的 CNB 日常开发环境中：

```sh
sh 10-env/build-image-transport.sh /tmp/image-transport-build
# 输出 vr-stream 和包含 commit、SDK digest、二进制 SHA256 的 build-info.json
```

或提交代码后调用独立流水线：

```sh
cnb build start-build --repo fu1fan/VibraReveal --branch codex/image-transport \
  --event api_trigger_image_transport --sync false
```

该流水线使用 `10-env/sdk.lock.json` 的固定 SDK，发布仅含二进制和身份文件的 OCI 制品：
`docker.cnb.cool/fu1fan/vibrareveal/image-transport:build-<CNB_BUILD_ID>`。
从 CNB 构建日志取得 digest 后 `docker create` / `docker cp` 提取文件；制品不是完整运行容器。
`vr-stream` 需部署到对应 Buildroot ABI 的板端。不能用 macOS 本机构建结果冒充 ARM64 Linux 产物。

## USB 与部署

当前系统须启用 FunctionFS / ConfigFS，USB-DP 口以 device 模式连接 Mac，
`cat /sys/class/udc/*/current_speed` 应为 `super-speed`。现有 NCM/ADB 可共存。
启动器只管理 `ffs.vri`，会暂时重新枚举整个 gadget；不影响 Wi-Fi。
Mac 需安装 `brew install libusb`，并允许 USB 附件连接（必要时解锁屏幕）。

从 CNB OCI 提取 `/vr-stream`、`/build-info.json` 和 `/scripts`，全部通过 Wi-Fi 上传：

```sh
scp vr-stream build-info.json scripts/* root@BOARD_WIFI_IP:/tmp/
ssh root@BOARD_WIFI_IP '
  mkdir -p /usr/local/sbin /usr/local/share/vibrareveal
  cp /tmp/vr-stream /usr/local/bin/
  cp /tmp/vr-usb-stream /tmp/vr-usb-stream-daemon /usr/local/sbin/
  cp /tmp/build-info.json /usr/local/share/vibrareveal/
  cp /tmp/S91vrstream /etc/init.d/
  chmod +x /usr/local/bin/vr-stream /usr/local/sbin/vr-usb-stream* /etc/init.d/S91vrstream
'
```

`/etc/init.d/S91vrstream start|stop|restart` 管理持续相机服务，默认 120 fps、无限帧，
等待接收端握手后才开始采集。会话失败/断开后自动清理并建立新会话；空闲等待每 60 秒重试。
有限帧测试或离线发送前先 `stop`，再运行单会话启动器。锁文件避免两个发送器争抢设备。
服务配置是本项目 ELF2 的 OV9281/USB 布局，移植其他板卡需修改启动脚本中的设备路径。

## 实时相机

先在板端确认 OV9281 对应视频节点和 sensor 节点，不依赖 `/dev/video-camera0`（可能指向 ISP）。
ELF2 当前连接为 `/dev/video11`、`/dev/v4l-subdev2`。采集速率由 sensor 的 frame interval 查询得到，
`--fps` 不覆盖真实相机速率。曝光、sensor FPS 和 RAW compact 模式由采集配置负责。
切换 OV9281 模式时还需同步 vertical_blanking；仅切换 FPS 而保留旧 vblank 会让实际采集速率仍接近 30 FPS。
例如在停止采集后，`v4l2-ctl -d /dev/v4l-subdev2 --set-subdev-fps pad=0,fps=120`，
再执行 `v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=vertical_blanking=110`。
30 FPS 模式对应 vblank=2856。最终以采集中读回的 frame interval 和帧时间戳为准。

```sh
# 板端，经 Wi-Fi 执行；启动器自动设置 RAW 非 compact 和 120 fps/vblank
/usr/local/sbin/vr-usb-stream --mode realtime --source camera --frames 7200 --timeout 30
# Mac：校验每帧并录像，或省略 --output 只测传输
vr-receive --usb --output capture.vri
# 送入 Metal；算法处理仍有自己的实时负载/丢帧策略
vibrareveal stream --usb true --low 3 --high 12 > amplified.vrm
```

USB 实时模式保持全尺寸、无逐帧 ACK，默认 32 帧环形缓冲。任何采集序号缺口或溢出都会报错。
传输无丢帧不等于 Metal 算法能全分辨率 120 fps 完成处理；采集验收应使用独立 `vr-receive`。

## 离线传输

```sh
# 板端：每帧 1280×800、16 位容器内10位数据；不按 --fps 限速
/usr/local/sbin/vr-usb-stream --mode offline --source file \
  --input /userdata/recording.raw --index /userdata/recording.idx \
  --width 1280 --height 800 --bits 10 --fps 120 --frames 0
# 无真实索引时省略 --index，协议会标记合成时间。

# Mac：先以磁盘/USB所允许的速度收完，再进行慢速算法处理
vr-receive --usb --output recording.vri
vibrareveal offline --archive recording.vri --low 3 --high 12 --spectrum spectrum.json > amplified.vrm
```

也可 `vibrareveal offline --usb true` 边收边暂存，但算法准备和暂存吞吐会影响接收速度。
最大传输吞吐应使用 `vr-receive` 独立接收，测图像通道上限时省略 `--output`（仍会校验每帧）。

## 验证

macOS 编译本地发送器只用于跨语言协议测试；正式 RK3588 程序由 CNB 编译：

```sh
clang++ -std=c++17 -O2 src/main.cpp src/camera.cpp src/bulk_transport.cpp -o /tmp/vr-stream-host
# 在仓库根目录执行，接收端路径来自 swift build --show-bin-path
python3 06-tests/image_transport.py /tmp/vr-stream-host /path/vr-receive /path/vibrareveal
```

`--source synthetic --frames N --fps F` 用于受控负载：实时按 F 生产，离线连续生产。
统计的 payloadMBps 是应用有效字节数/秒，不包含协议头，也不是 USB PHY 速率。

硬件验收与构建身份见 [正式 Bulk 图像验收](../../08-docs/validation/usb-bulk-images-2026-10-31.md)。

## 板端 Vulkan ROI 实时运动放大

新增 `vr-motion`，直接读取 V4L2 GREY/Y10，截取指定区域后上传 GPU。
Vulkan 1.0 compute 完成三层（可配 1～8 层）高斯/拉普拉斯金字塔、Riesz 近似局部相位、
因果 IIR 带通、相位旋转和重建。与 Mac 端采用同一算法结构；输出是 ROI 尺寸的灰度图像。
`alpha` 是相位增益，不是标定后的物理位移倍率。边缘采用 reflect101；允许奇数尺寸和单像素轴。

### 构建与启动

默认构建保持原有采集发送器；启用新目标需要 SDK sysroot 内的 Vulkan headers/loader，
以及**构建主机**上的 `glslangValidator`。在 CNB SDK 环境内执行：

```sh
elf2-cmake -S 03-apps/rk3588 -B /tmp/vr-motion-build \
  -DCMAKE_BUILD_TYPE=Release -DVR_BUILD_VULKAN_MOTION=ON -DBUILD_TESTING=OFF
elf2-sdk run cmake --build /tmp/vr-motion-build --parallel 4
```

部署 `vr-motion` 和 `motion.comp.spv` 到对应 ABI 的 RK3588 系统，通过 `--shader` 指定后者。
也可 `cmake --install`；默认 shader 安装到 `${prefix}/share/vibrareveal/motion.comp.spv`。
此变更未修改现有 OCI transport 制品流水线，旧制品中不包含新程序。

相机须已设置 RAW 非 compact、真实 sensor FPS 和对应 vblank（见上文），并停止占用同一
V4L2 节点的 `vr-stream` 服务。`--fps` 是算法/输出帧率，**不会修改传感器寄存器**。
例如传感器为 120 FPS，处理中心附近 256×256 ROI、目标 30 FPS：

```sh
vr-motion --source camera --device /dev/video11 --sensor /dev/v4l-subdev2 \
  --width 1280 --height 800 --bits 10 \
  --x 512 --y 272 --roi-width 256 --roi-height 256 \
  --fps 30 --low 3 --high 12 --alpha 12 --levels 3 --frames 900 \
  --shader /usr/local/share/vibrareveal/motion.comp.spv --output amplified.vrv
```

参数在启动时设置；更改 ROI/帧率需重启会话。`--frames 0` 表示相机持续采集。
目标 FPS 必须等于真实 sensor FPS 或是它的整数分频（120 → 120/60/40/30 等），
否则返回 `UNSUPPORTED_FPS`。抽帧不提供时间抗混叠；运动频率应低于目标 Nyquist 频率。
每一个原始采集帧仍检查序号和时间戳，主动抽帧不会掩盖采集丢帧。
ROI 限制为 1,048,576 像素，还受设备 storage-buffer 限制；不会自动缩小区域或降低帧率。

不指定 `--output` 时只处理并检查实时预算。`--output -` 写 stdout，可连接自定义显示/编码进程；
本程序不含 HDMI 窗口、视频编码器，也不改变现有 VRI1 USB 协议。
文件使用独立的 `VRV1` 帧格式，每帧 40 字节小端头 + `width*height` 个 float32 像素：

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | char[4] | `VRV1` |
| 4, 8, 12 | uint32 | ROI 宽、高、payload 字节数 |
| 16 | uint64 | 原始相机帧序号（主动抽帧后允许间隔） |
| 24 | uint64 | 采集 CLOCK_MONOTONIC 纳秒 |
| 32, 36 | uint32 | 目标 FPS 分子、分母 |
| 40 | float32[] | 行优先的放大灰度，标称 0～1，可能超范围；显示时自行裁剪 |

错误时最后一帧可能只有部分字节，读取者必须检查完整性。已有文件不会被覆盖。

### 实时失败行为

应用事件在 stderr 输出 JSON Lines（`ready` / `complete` / `error`）；Mali 驱动可能夹杂普通诊断行，
接收端按 `{` 开头识别应用事件。`--output -` 会将驱动 stdout 隔离到 stderr，避免污染二进制流。
错误包含 `code`、`message`，
退出码 2 表示明确的运行/配置错误，1 表示其他异常。采集线程保留至多两个待处理帧。

| 错误码 | 含义 |
| --- | --- |
| `GPU_DEADLINE_EXCEEDED` | Vulkan 提交至 fence 完成超过 `1000/fps` ms，建议减小 ROI 或 FPS |
| `PROCESSING_DEADLINE_EXCEEDED` | ROI 转换、GPU、读回合计超过一帧预算 |
| `CAPTURE_FRAME_DROPPED` | V4L2 原始帧序号缺口 |
| `CAPTURE_TIMING_ERROR` | 原始采集间隔偏离 sensor 标称值超过 20% |
| `PROCESSING_QUEUE_OVERFLOW` | GPU/CPU/输出消费速度不足，两个待处理槽已满 |
| `OUTPUT_DEADLINE_EXCEEDED` | 输出写入超过一帧预算 |
| `PIPELINE_DEADLINE_EXCEEDED` | 处理与输出的总时间超过一帧预算 |
| `VULKAN_ERROR` | Vulkan 返回错误，包括 device lost |

超预算立即失败，不继续输出损坏的时域历史。GPU 错误报告的是包含驱动/队列等待的提交耗时，
不能据此把所有超时都归因为 GPU 算力；采集、CPU、输出问题分别报告。
Vulkan 无法取消已提交任务：报告错误后析构仍需等待 GPU 安全释放资源，驱动卡死可能延迟退出。
普通文件系统写入同样不能保证内核调用有硬实时上限；管道使用 nonblocking + poll 截止时间。
每帧预算包括首帧实际 dispatch，驱动首次执行过慢也会报错。

### 验证

在装有 Vulkan loader/ICD、glslang、CMake 和 NumPy 的 Linux 测试环境：

```sh
cmake -S 03-apps/rk3588 -B /tmp/vr-motion-test -DVR_BUILD_VULKAN_MOTION=ON -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/vr-motion-test --parallel 4
ctest --test-dir /tmp/vr-motion-test --output-on-failure
python3 06-tests/vulkan_motion.py /tmp/vr-motion-test/motion-test.f32
python3 06-tests/vulkan_motion_cli.py /tmp/vr-motion-test/vr-motion /tmp/vr-motion-test/motion.comp.spv
```

测试包含非零增益 64 帧与独立 NumPy 参考比较、alpha=0 重建、奇数/细长 ROI、输入 stride、
错误参数、损坏帧、帧序号/时间戳中断及会话失败后拒绝继续。
软件 Vulkan 只能验证功能，不代表 Mali-G610 性能；正式板上应从小 ROI 起测，记录无错误的持续运行区间，
再逐步增加尺寸/帧率，并确认负载超过极限时收到明确错误。已完成的 Mali-G610 短时实机验收见 [2026-10-31 记录](../../08-docs/validation/vulkan-roi-motion-2026-10-31.md)；
其中 128×128@120 和 256×256@60 的短测通过，不能据此保证长时间持续吞吐或热稳定性。
