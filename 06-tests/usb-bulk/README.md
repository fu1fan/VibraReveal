# USB Bulk 带宽基准

这是一套诊断工具，不是图像传输协议。RK3588 使用 FunctionFS + Linux AIO 向 Bulk IN
端点持续写入预先生成的数据；Mac 使用 libusb/IOKit 的异步请求直接接收。
不经过 NCM、IP、TCP/UDP，不写接收到的数据文件，不执行逐帧 ACK 或 CRC。

## 构建

板端程序 `vr-bulk-source` 由现有 `api_trigger_image_transport` CNB 流水线编译，
和 `vr-stream` 一同放入 OCI 制品；`build-info.json` 的 `bulk_benchmark_sha256` 标识它。
仍通过 Wi-Fi 上传至板端 `/tmp/vr-bulk-source`，无需在板端安装 libaio。

Mac 使用已安装的 libusb：

```sh
clang++ -std=c++17 -O2 -Wall -Wextra 06-tests/usb-bulk/receive.cpp \
  $(pkg-config --cflags --libs libusb-1.0) -o /tmp/vr-bulk-receive
```

## 临时配置（仅适用于当前 ELF2 gadget）

以下板端操作全部通过 Wi-Fi SSH 执行。先停止图像会话；不要通过即将重新枚举的 USB 网络管理板端。
不要覆盖已存在的 `ffs.vrbench` 实例或其他使用中的 FunctionFS 挂载。
现有 NCM 和 ADB 接口保留，但增加接口需要短暂 USB 重枚举。

第一个 SSH 终端：

```sh
mkdir /sys/kernel/config/usb_gadget/rockchip/functions/ffs.vrbench
mkdir -p /tmp/vr-bulk-ffs
mount -t functionfs vrbench /tmp/vr-bulk-ffs
chmod +x /tmp/vr-bulk-source
/tmp/vr-bulk-source /tmp/vr-bulk-ffs 262144 32
```

等到输出 `descriptors_ready` 后，在第二个 SSH 终端绑定接口：

```sh
cat /sys/kernel/config/usb_gadget/rockchip/UDC > /tmp/vr-bulk-original-udc
echo '' > /sys/kernel/config/usb_gadget/rockchip/UDC
ln -s /sys/kernel/config/usb_gadget/rockchip/functions/ffs.vrbench \
  /sys/kernel/config/usb_gadget/rockchip/configs/b.1/f-ffs.vrbench
cat /tmp/vr-bulk-original-udc > /sys/kernel/config/usb_gadget/rockchip/UDC
```

Mac 保持解锁。接收器只匹配 VID/PID `2207:0012` 下 class/subclass/protocol
为 `ff/56/01` 的专用接口，不占用 ADB 或 NCM。实际 IN endpoint 地址由组合设备分配。
SuperSpeed 描述符使用 1024 字节包和 `bMaxBurst=15`；后者与现有 NCM 设置相同。

## 测试

```sh
# 参数：每个请求字节数、Mac 并发请求数、计量秒数（另有 3 秒预热）
/tmp/vr-bulk-receive 262144 1 15
/tmp/vr-bulk-receive 262144 16 60
/tmp/vr-bulk-receive 1048576 16 20
```

板端请求大小/队列深度为启动发送器时给定的值，和 Mac 请求大小/队列深度独立。
本次固定板端 256 KiB × 32，仅扫描 Mac 参数。单端总缓冲限制为 64 MiB。
统计按完成回调中的实际接收字节数和单调时钟计算，使用十进制 MB/s。
稳态窗口边界存在在途请求归属误差，短测数字不可作为精密物理线速测量。

每轮在预热阶段核对首个完整接收缓冲中的固定非零模式；计量期间不逐字节校验。
`errors` 为 libusb 报告的传输错误，`shortReads` 为未达到请求长度的完成数。
USB 底层重试不可由这些指标直接观测。重复模式不能验证数据块序号、帧丢失或顺序，
因此本测试不能代替真实相机 120 FPS、图像完整性或端到端延迟验收。

## 恢复

先在 Wi-Fi SSH 中解绑 UDC，取消设备端排队请求；发送器会收到 DISABLE/关闭完成并退出。
不要在主机已停止读取时只给发送器发终止信号：AIO 销毁可能等待尚未消费的请求。

```sh
echo '' > /sys/kernel/config/usb_gadget/rockchip/UDC
rm /sys/kernel/config/usb_gadget/rockchip/configs/b.1/f-ffs.vrbench
# 确认 vr-bulk-source 已退出后再执行：
umount /tmp/vr-bulk-ffs
rmdir /sys/kernel/config/usb_gadget/rockchip/functions/ffs.vrbench
/usr/bin/usbdevice restart
```

Mac 再运行 `python3 09-tools/check-usb-link.py` 确认 NCM 路由恢复。
整个测试不修改 `/etc/.usb_config`、USB 启动脚本或 Wi-Fi 配置。

正式图像协议验收使用 [`image_acceptance.py`](image_acceptance.py)，不是本页的诊断字节流工具。
