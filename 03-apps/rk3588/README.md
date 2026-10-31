# RK3588 图像采集与高速传输

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
