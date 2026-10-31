# USB 图像传输验收 · 2026-10-31

## 已完成

- 开发前从 CNB 拉取 VibraReveal main `d743b7f`；本地 `.gitignore` 未改动。
- macOS Swift/Xcode/Metal 可用。现有后端 21 项测试、新增协议 4 项测试通过。
- 跨语言集成通过：RAW+idx 逐字节校验、奇数尺寸/行 padding、119.88 FPS 元数据、59.94 FPS 实时节奏、慢接收端空间降采样及队列丢帧、截断/CRC/异常断连，以及原始录像直接接入 Metal 离线算法。
- CNB 环境自检 `cnb-e0t-1k687u4jo` 成功：固定 SDK 的 CMake/Vulkan 交叉编译和缓存复用。
- 应用构建 `cnb-8df-1k68a5e5d` 成功，应用 commit `fe1c9a24e3f86248958b7358aba04128df1ca148`。
  ARM64 程序 SHA256：`181d740bec5bf43484e4c6aed619634ac2eb6780b97816116b41707e8cc149cf`。
  制品：`docker.cnb.cool/fu1fan/vibrareveal/image-transport@sha256:b6bce947e0647532d014edbfb0f09aace18254d7f19b9e5b950220c9eb766ad5`。
- 首轮 NCM 系统构建 `cnb-anu-1k6891q58` 成功，系统 commit `123806998cae13be755c5fb87a562d61fbe7ba7e`。
  boot.img SHA256：`49b1e2f446de319a2465bbf5d7800b313eb1299572ee8f82bb8c7038a22abe7d`。
  update.img SHA256：`e134e2e8d2024557ad656604d2bb362d762ee542ad128ff3d0f31aba83c3516b`。
- boot.img 经 Wi-Fi 上传、写入 boot 分区并读回校验一致；未整包重刷 rootfs。
  原 boot 分区备份在板端 `/userdata/usb-ncm-backup-20261031/boot.img`，SHA256：
  `36aed33f7709956842154b1cf904f44943d0848fba7c1f5d495136bf08b8d21b`。
- 板端已启动新 Linux 5.10.209 内核，Wi-Fi `192.168.8.204` 的 SSH 正常。
  USB 物理链路为 SuperSpeed；Mac 已加载 `AppleUSBNCMControl/AppleUSBNCMData`。
- NCM 启动联调修正了 `ifname` 必须含 `%d` 和 udev 重复 update 的幂等性问题。
  修正后板端 `vrusb0=192.168.77.1/24`，独立 dnsmasq 已启动；最终源码 `2c63f6606fb04675c9024249c53daf816a05f8d1` 的 CNB 构建 `cnb-pn9-1k689qu0r` 已成功，51 项 Linux 回归、系统镜像、SDK 导出和制品发布全部通过。
- 相机为 `/dev/video11`，sensor `/dev/v4l-subdev2` 当前声明 1280×800@30 FPS。
  关闭 RKCIF compact 后为 Y10，stride=2560，单帧 2,048,000 字节；板端 30 帧 V4L2 采集成功。

## 尚未完成的硬件验收

Mac 当时锁屏，configd InterfaceNamer 明确记录：`Console locked, network interface[new] ignored`。
因此 NCM 驱动已匹配，但还没有注册 BSD 网卡。不能据此声称 USB 图像链路已通过或给出带宽结果。

解锁后先运行：

```sh
python3 09-tools/check-usb-link.py
```

检查必须确认目的地址实际经 Apple NCM 接口路由。仅 ping 成功不够：本次曾观察到
192.168.77.1 经 Wi-Fi 默认网关返回 ICMP，不能作为 USB 连通证据。

剩余验收：真实相机端到端序号/时间戳及 CRC；30/120 FPS 实时负载；离线最大持续吞吐、写盘吞吐及字节一致性；USB 重连及重启后恢复。

## 部署边界与回滚

管理和程序/配置上传始终使用 Wi-Fi。USB 只用于图像通道；没有通过 ADB 部署或代替 Wi-Fi SSH。
板上程序持久安装为 `/usr/local/bin/vr-stream`，身份文件为 `/usr/local/share/vibrareveal/build-info.json`。
USB DHCP 不发布默认网关或 DNS，Mac 的 Wi-Fi 管理连接保持独立。

本次是 boot+USB 配置的增量部署，原 `/etc/elf2-build-info.json` 仍表示旧 rootfs 身份，不能视为整个系统都刷成了新版本。
回滚需要恢复备份 boot、原 `/usr/bin/usbdevice`，删除本次新增 `/etc/.usb_config` 和 USB 专用 dnsmasq 配置，再重启。
执行前校验备份哈希，并保证 Wi-Fi 可达；不要覆盖 rootfs 或用户数据分区。
