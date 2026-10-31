# RK3588 ↔ macOS 图像协议 VRI1

实现：[发送端](../03-apps/rk3588)、[Swift 接收库](../03-apps/macos/Sources/VibraRevealTransport/FrameReader.swift)。
正式图像通道为 USB 3 **自定义 Bulk**；SSH、程序部署和文件上传继续走 Wi-Fi。
`vr-stream` 默认 `--transport bulk`。NCM/TCP 仅作为显式选择的兼容通道（`--transport tcp`）。
两种通道共享下面的 VRI1 帧格式，Bulk 会设置 flags bit2，旧接收端会拒绝未知标志。

## Bulk 会话与流水

FunctionFS 接口 class/subclass/protocol 为 `ff/56/02`，有 Bulk IN 和 OUT 各一个。
Mac 按接口描述符查找端点，不硬编码组合设备中的接口号/实际端点号；要求 SuperSpeed。
板端程序先提交描述符，启动器再加入 gadget 并重新枚举。Mac 提前挂好 IN 请求，
通过 OUT 发送 8 字节 ASCII `VRSTART1`，板端才启动相机并发送 HELLO。

- **realtime**：相机按传感器帧率采集；文件/合成源按指定 FPS 生产。16 个 V4L2 MMAP buffer 及时回收，独立采集线程复制到默认 32 帧的有界环形缓冲，USB 发送线程计算 CRC 并提交请求。保持原始尺寸，不降分辨率、不覆盖旧帧；采集序号缺口或环形缓冲溢出会终止会话并报错。
- 板端默认 `32 × 256 KiB` Linux AIO 请求；Mac `16 × 256 KiB` libusb 异步接收槽，按提交顺序拼接字节流，槽位消费后才重用。无逐帧 ACK。实时每帧尾部提交不足一块的请求，避免等待下一帧填满；USB 请求边界不等于 VRI1 帧边界。
- **offline**：读取已有 RAW/索引，不按 FPS 限速；连续填充异步 USB 队列，通过有限队列自然背压。保持字节、顺序和索引时间；内存不随文件长度增长。小块合并以提高吞吐。
- 接收端逐帧验证 CRC、尺寸、连续序号和时间，END 核对帧数与总字节数，才通过 OUT 返回 `VRDONE01`。实时和离线都必须以完整 END 结束；断线、超时、CRC 错误、溢出均不算成功。
- 默认 32 帧相机环形缓冲约可容纳 265 ms 的 120 fps 数据，但不能保证任意长的主机停顿或持续欠速不丢采集数据。缓冲满时明确失败，不把不完整片段伪装成成功录像。环形缓冲配置限制为 256 MiB；此外还有 V4L2、USB 和当前处理帧的内存。
- 不支持半帧续传。启动器/服务清理旧 FunctionFS 会话后重新枚举，接收端需建立新会话。`--timeout` 是操作截止时间，也限制初始等待。

## TCP 兼容模式

USB NCM 默认板端 `192.168.77.1`、Mac DHCP `192.168.77.2`，端口 5910。
DHCP 不提供默认网关或 DNS。TCP 无加密/认证；默认只监听 loopback，使用 USB 时显式绑定 USB 地址。
TCP 实时模式保留原先的单帧待发队列、逐帧 ACK 和自适应缩放，过载时可能替换待发帧；
`--adaptive false` 仅禁止缩放，不改变丢帧策略。TCP 离线模式保持尺寸、连续发送且无逐帧 ACK。
这些兼容策略不用于正式 Bulk 通道的无丢帧验收。

## 字节布局

所有整数为**小端**，无结构体填充。每个记录是 80 字节头，紧接 `payloadBytes` 字节。

| 偏移 | 类型 | 内容 |
|---|---|---|
| 0 | 4 bytes | ASCII `VRI1` |
| 4 | u16 | version=1 |
| 6 | u16 | kind：1 HELLO，2 FRAME，3 END |
| 8 | u32 | flags：bit0 实时；bit1 时间由序号/FPS 合成；bit2 无逐帧 ACK 的固定尺寸流水；其他位必须为 0 |
| 12 | u32 | payloadBytes；HELLO/END 必须为 0 |
| 16/20/24/28 | u32 ×4 | 传输 width/height/stride/validBits |
| 32 | u64 | FRAME 采集序号；END 已发送帧数；HELLO 为 0 |
| 40 | u64 | FRAME 单调采集时间 ns；END 累计 payload 字节数；HELLO 为 0 |
| 48/52 | u32 ×2 | 源 FPS 分子/分母（不能用网络到达速率替代） |
| 56 | u64 | 累计发送端待发队列替换数；不含传感器/驱动自身缺口 |
| 64 | u32 | FRAME payload 的 IEEE CRC32（与 zlib 一致）；其他记录为 0 |
| 68/72 | u32 ×2 | 原始 sourceWidth/sourceHeight |
| 76 | u32 | 保留，必须为 0 |

每轴 1...65535，单帧默认上限 256 MiB；接收端在分配 payload 前验证尺寸和乘积。
`bits=8` 为 GRAY8；10/12/16 为低位对齐 uint16 LE，允许行 padding。
FRAME payload 必须恰好为 stride×height，含最后一行 padding。当前不支持 Bayer、RGB、MIPI packed RAW10。
相机会读取单平面的 V4L2 multiplanar GREY/Y10，验证 monotonic 标记，并处理 32 位采集序号回绕。
RKCIF compact 模式必须先关闭，不能把厂商压缩布局误标成 uint16。

HELLO 固定会话模式、源尺寸、位深和 FPS。仅 TCP 实时 FRAME 的尺寸/stride 可以下降；Bulk 和离线必须保持与 HELLO 一致。
源格式或 FPS 改变须结束旧会话并重新建立连接。序号和时间必须严格递增；Bulk 和离线还要求序号连续。
接收端不把机器间时钟差解释为单向延迟，也不使用墙钟重写采集时间。

仅 TCP 实时 ACK 是 8 字节 u64 LE 原始帧序号，在 payload 校验和可选录像写入后返回。
END ACK 是 ASCII `VRDONE01`，只有帧数和总字节数一致才返回。
录像模式在 END ACK 前同步录像文件，但 `.partial` 到最终文件名的发布仍可能失败；最终以接收命令退出码为准。
发送/接收的每次 I/O 使用绝对截止时间（默认 15 秒）；接收端突然中断不是正常 EOF。

## 录像与文件索引

`.vri` 文件保存 HELLO、每帧头+payload、END 的完整字节流；包含真实时间、序号和尺寸变化。
接收失败保留 `.vri.partial`，只在完整验证后以不覆盖已有文件的方式发布 `.vri`。
`vr-receive --archive` 可以重新验证，`vibrareveal ... --archive` 可直接处理。

发送端 `--source file` 读取固定尺寸无头 `.raw`；可提供 `--index capture.idx`：
每帧恰好 16 字节，依次为 u64 LE sequence 和 u64 LE monotonicTimestampNS。
没有索引时明确设置 syntheticTimestamps 标志，时间为 `sequence/fps`，不是实测传感器时间。
`--frames 0` 表示文件至 EOF；指定有限帧数时，文件短于该数量视为错误，较长则仅发送指定前缀。
不包含 IMU、曝光控制、ROI 控制或硬件时钟同步；这些不占用未定义的保留字段。
