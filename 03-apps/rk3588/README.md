# RK3588 图像采集与高速传输

`vr-stream` 是 C++17 单会话发送器，支持 V4L2 相机、已有 RAW 文件及合成负载。
图像走 USB NCM；管理和部署走 Wi-Fi。协议与队列策略见 [VRI1](../../05-protocol/README.md)。

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

## USB 前置条件

系统子模块需启用 `CONFIG_USB_CONFIGFS_NCM=y`，并包含 NCM gadget、USB 专用 DHCP 配置。
板端 `vrusb0=192.168.77.1/24`；Mac 自动获取 `192.168.77.2`，没有 USB 默认路由或 DNS。
若本地网络已使用 192.168.77.0/24，先为 USB 选择不冲突的网段并同步两端配置。
Mac 首次注册新网卡时需解锁屏幕。测试前运行 `python3 09-tools/check-usb-link.py`，
确认路由实际经过 Apple NCM 网卡，不能只凭 ping 结果判断。
USB 热插拔应重新获得 DHCP，Wi-Fi 管理地址不变；链路中断会终止当前图像会话。
板端 `cat /sys/class/udc/*/current_speed` 应为 `super-speed`。

部署仍经 Wi-Fi，例如：

```sh
scp /path/to/cnb/vr-stream root@BOARD_WIFI_IP:/tmp/vr-stream
ssh root@BOARD_WIFI_IP 'chmod +x /tmp/vr-stream'
```

## 实时相机

先在板端确认 OV9281 对应视频节点和 sensor 节点，不依赖 `/dev/video-camera0`（可能指向 ISP）。
ELF2 当前连接为 `/dev/video11`、`/dev/v4l-subdev2`。采集速率由 sensor 的 frame interval 查询得到，
`--fps` 不覆盖真实相机速率。曝光、sensor FPS 和 RAW compact 模式由采集配置负责。
切换 OV9281 模式时还需同步 vertical_blanking；仅切换 FPS 而保留旧 vblank 会让实际采集速率仍接近 30 FPS。
例如在停止采集后，`v4l2-ctl -d /dev/v4l-subdev2 --set-subdev-fps pad=0,fps=120`，
再执行 `v4l2-ctl -d /dev/v4l-subdev2 --set-ctrl=vertical_blanking=110`。
30 FPS 模式对应 vblank=2856。最终以采集中读回的 frame interval 和帧时间戳为准。

```sh
# 板端：先记录原值；采集停止后可恢复。不要在其他采集程序运行时切换。
cat /sys/devices/platform/rkcif-mipi-lvds5/compact_test
printf '0 0 0 0\n' > /sys/devices/platform/rkcif-mipi-lvds5/compact_test
/tmp/vr-stream --bind 192.168.77.1 --mode realtime --source camera \
  --device /dev/video11 --sensor /dev/v4l-subdev2 --width 1280 --height 800 --bits 10 --frames 300

# Mac：原始图像录像（校验后写盘，不做运动放大）
vr-receive --host 192.168.77.1 --output capture.vri
# 或实时送入 Metal；频带必须满足实际 FPS 的 Nyquist 限制
vibrareveal stream --connect 192.168.77.1 --low 3 --high 12 > amplified.vrm
```

默认在启动后 15 秒内等待 Mac 连接；需要更长操作窗口可同时在两端提高 `--timeout`。
`--frames 0` 可持续采集；停止接收会使发送端报断连并回收采集缓冲。
实时改变尺寸会重置 Metal 时间历史；固定 ROI 要适配实际传输尺寸，默认全帧最稳妥。

## 离线传输

```sh
# 板端：每帧 1280×800、16 位容器内10位数据；不按 --fps 限速
/tmp/vr-stream --bind 192.168.77.1 --mode offline --source file \
  --input /userdata/recording.raw --index /userdata/recording.idx \
  --width 1280 --height 800 --bits 10 --fps 120 --frames 0
# 无真实索引时省略 --index，协议会标记合成时间。

# Mac：先以磁盘/USB所允许的速度收完，再进行慢速算法处理
vr-receive --host 192.168.77.1 --output recording.vri
vibrareveal offline --archive recording.vri --low 3 --high 12 --spectrum spectrum.json > amplified.vrm
```

也可 `vibrareveal offline --connect ...` 边收边暂存，但算法准备和暂存吞吐会影响接收速度。
最大传输吞吐应使用 `vr-receive` 独立接收，测网络上限时省略 `--output`（仍会校验每帧）。

## 验证

macOS 编译本地发送器只用于跨语言协议测试；正式 RK3588 程序由 CNB 编译：

```sh
clang++ -std=c++17 -O2 src/main.cpp src/camera.cpp -o /tmp/vr-stream-host
# 在仓库根目录执行，接收端路径来自 swift build --show-bin-path
python3 06-tests/image_transport.py /tmp/vr-stream-host /path/vr-receive /path/vibrareveal
```

`--source synthetic --frames N --fps F` 用于受控负载：实时按 F 生产，离线连续生产。
统计的 payloadMBps 是应用有效字节数/秒，不包含协议头，也不是 USB PHY 速率。

硬件验收与构建身份见 [2026-10-31 记录](../../08-docs/validation/image-transport-2026-10-31.md)。
