# USB 图像传输验收 · 2026-10-31

## 已完成

- 开发前从 CNB 拉取 VibraReveal main `d743b7f`；本地 `.gitignore` 未改动。
- macOS Swift/Xcode/Metal 可用。现有后端 21 项测试、新增协议 4 项测试通过。
- 跨语言集成通过：RAW+idx 逐字节校验、奇数尺寸/行 padding、119.88 FPS 元数据、59.94 FPS 实时节奏、慢接收端空间降采样及队列丢帧、截断/CRC/异常断连，以及原始录像直接接入 Metal 离线算法。
- CNB 环境自检 `cnb-e0t-1k687u4jo` 成功：固定 SDK 的 CMake/Vulkan 交叉编译和缓存复用。
- 应用构建 `cnb-69p-1k68cp87u` 成功，应用 commit `ae1da870c0df5b7157321e4b3941fb62faca8ca4`。
  ARM64 程序 SHA256：`5c9b55daa3b31425b0439aba959d3a5223c8b9de2afe4c5ed14108b4fad38f08`。
  制品：`docker.cnb.cool/fu1fan/vibrareveal/image-transport@sha256:f6c94122e4cf8ad27afcfc3918728fe38556fdd73b0a335367f9650037e215b5`。
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

- 后续相机修复系统 commit `bc78f9d2f4a9e48d69f8b1dff38a3dd905f374bb`，CNB `cnb-ckb-1k68bv0hh` 编译、验证、发布成功。
  新 boot SHA256 `a5bc68a9078faf36c01d54f3babe317f6e007bf4e9d7e0f822774dd2e3cd99c2`，经 Wi-Fi 上传、写入并读回一致。

## USB 实机验收

Mac 解锁并重新枚举后，AppleUSBNCMData 注册 `en7=192.168.77.2`，板端 carrier=1、SuperSpeed。
`check-usb-link.py` 验证 192.168.77.1 经 en7，Wi-Fi 默认路由仍经 en0/192.168.8.1。
检查器另用板端 Wi-Fi 地址做负例，正确拒绝将 Wi-Fi 路由计作 USB。
修正了 ioreg XML 查询缺少 `-l` 时不包含子接口 BSD Name 的问题。

| 测试 | 结果 |
| --- | --- |
| 离线合成源，512 帧，1.049 GB | 205.17 MB/s，CRC 通过，无丢帧；包含板端合成图像开销 |
| 离线真实 RAW，150 帧，307.2 MB，Mac 写盘 | 302.05 MB/s，无丢帧，重组 payload 与板端 SHA256 一致 |
| 离线 RAW 重复数据，1200 帧，2.4576 GB，Mac 写盘 | 318.26 MB/s，7.722 秒，无丢帧，全部 CRC 和整段 SHA256 通过 |
| 真实相机 1280×800 Y10、30 FPS，150 帧 | 61.52 MB/s，序号 0…149、真实采集时间戳，实测约 30.048 FPS，无丢帧 |
| 真实相机 → USB → Metal 实时处理，30 FPS，150 帧 | submitted=completed=150，dropped=failed=0 |
| 真实离线录像 → Metal 零相位处理，150 帧 | completed=150，dropped=0 |
| 最终版本，真实相机 120 FPS，7200 个源帧，约 60 秒，Mac 写盘 | 收到 7188 帧，显式丢弃 12 帧（0.17%）；12 帧全分辨率后稳定为 640×400，剩余 7176 帧，未继续降档；overloaded=false |
| 最终版本离线 RAW 150 帧，Mac 写盘 | 305.19 MB/s，无丢帧，CRC 与板端整段 SHA256 一致 |
| USB gadget 软件重枚举后重新建立相机会话 | DHCP/USB 路由恢复，30 帧全部收到，无丢帧；Wi-Fi SSH 保持可用 |

速率单位为十进制有效 payload MB/s，包含 CRC、协议交互，以及注明的写盘/同步开销；
不是 USB 5 Gbit/s 标称线速，也不能代表任意磁盘、图像尺寸或无限时长的最低保证。
上述长文件来自板端 tmpfs；持久存储读速慢时离线吞吐会受读盘限制。

120 FPS 录像包含真实采集时间戳，按序号/时间差测得 sensor 约 120.622 FPS；
协议声明值为驱动查询所得 120 FPS。实时模式优先限制延迟，不能保证在系统调度抖动时零丢帧；
需要每帧完整保留时使用离线模式。这里验证了约一分钟的实时运行，未验证全天稳定性或物理拔插。
最终离线 RAW SHA256：`40739b93c384c3c5bf546571da545e24a08136f3d7fa7c0780e815a6fcdd6fd8`。

150 帧真实 RAW SHA256：`1aa3196e58daad41945842e6e762d2037bf27722c98e3ac4d53ef0fff7f01b60`。
1200 帧 payload SHA256：`e2e91bbba24704b546ee5f64b532e4f499046f53da196a5f2dd38ad79cef4393`。

首次实机采集暴露 OV9281 首次 STREAMON 前 `cur_fps` 未初始化；驱动已修正为初始化当前模式帧率和 VTS。
120 FPS 实测还暴露了对瞬时延迟过于敏感的降采样条件：现改为持续超出 90% 帧周期预算触发，零星队列丢帧不累计触发降档，
并用 ARM NEON 优化面积平均；跨语言测试独立核对降采样像素值。

新版内核重启后 USB DHCP 和 Wi-Fi SSH 均自动恢复；未调用 sensor 设置操作的首次采集
150 帧/30 FPS、61.65 MB/s、无丢帧，验证了帧率初始化修复。

## 部署边界与回滚

管理和程序/配置上传始终使用 Wi-Fi。USB 只用于图像通道；没有通过 ADB 部署或代替 Wi-Fi SSH。
Mac 程序安装在 `~/.local/lib/vibrareveal/`，命令入口为 `~/.local/bin/vr-receive` 和
`~/.local/bin/vibrareveal`，Metal 资源 bundle 一并安装；安装后的 Metal benchmark 和录像校验通过。
板端测试结束后恢复 sensor 30 FPS、vblank=2856，RAW compact 保持关闭。
板上程序持久安装为 `/usr/local/bin/vr-stream`，身份文件为 `/usr/local/share/vibrareveal/build-info.json`。
USB DHCP 不发布默认网关或 DNS，Mac 的 Wi-Fi 管理连接保持独立。

本次是 boot+USB 配置的增量部署，原 `/etc/elf2-build-info.json` 仍表示旧 rootfs 身份，不能视为整个系统都刷成了新版本。
回滚需要恢复备份 boot、原 `/usr/bin/usbdevice`，删除本次新增 `/etc/.usb_config` 和 USB 专用 dnsmasq 配置，再重启。
执行前校验备份哈希，并保证 Wi-Fi 可达；不要覆盖 rootfs 或用户数据分区。
