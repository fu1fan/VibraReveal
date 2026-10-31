# RK3588 ↔ macOS 图像协议 VRI1

实现：[发送端](../03-apps/rk3588)、[Swift 接收库](../03-apps/macos/Sources/VibraRevealTransport/FrameReader.swift)。
图像数据走 USB 3 NCM 的独立 TCP 连接；SSH、程序部署和文件上传继续走 Wi-Fi。
默认 USB 地址为板端 `192.168.77.1`、Mac DHCP `192.168.77.2`，端口 5910。
USB DHCP 不提供默认网关或 DNS。协议也能在其他可靠字节流上运行。
TCP 无加密/认证；发送端默认只监听 loopback，使用 USB 时显式绑定板端 USB 地址，不绑定所有网卡。

## 模式

- **realtime**：传感器按自身帧率采集；文件/合成源按指定 FPS 定时生产。生产线程只保留一帧待发，另有一帧发送中。网络线程不持有 V4L2 DMA buffer。接收端校验后逐帧 ACK，限制 TCP 积压并提供传输耗时反馈。
- 实时耗时预算是一个源帧周期。连续 max(3, ceil(FPS/10)) 次 EMA 超预算，或距上次调整至少发送 ceil(FPS/4) 帧且队列累计丢弃至少 3 帧，将宽高各缩小约一半，使用面积平均，覆盖全视场；源帧率、采集序号和时间保持不变。会话只降不升，最低降至两轴不超过 32；仍不够时报告 overloaded。禁用 `--adaptive false` 可保持原尺寸，但不足的链路仍会明确丢弃待发帧。
- 丢帧是故障/过载信息，不伪装成降采样率。接收端报告序号缺口，Metal 实时滤波器按现有规则重置；不应把有缺口片段用于连续振动测量。
- **offline**：读取已录制文件，无 FPS 节流，无逐帧 ACK；连续写入 TCP，利用接收窗口实现背压，保持每个原始字节、尺寸、顺序和索引时间。速率由 USB、TCP、磁盘和接收端决定；不承诺达到 USB 标称物理速率。内存使用不随文件长度增长。
- 断线/CRC 错误/超时终止整个会话，不能在半帧处继续。当前不支持断点续传；重连须重新启动发送会话。完整离线确认只在接收端验证结束计数后发出。

## 字节布局

所有整数为**小端**，无结构体填充。每个记录是 80 字节头，紧接 `payloadBytes` 字节。

| 偏移 | 类型 | 内容 |
|---|---|---|
| 0 | 4 bytes | ASCII `VRI1` |
| 4 | u16 | version=1 |
| 6 | u16 | kind：1 HELLO，2 FRAME，3 END |
| 8 | u32 | flags：bit0 实时；bit1 时间由序号/FPS 合成；其他位必须为 0 |
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

HELLO 固定会话模式、源尺寸、位深和 FPS。实时 FRAME 的尺寸/stride 可以下降；离线必须保持与 HELLO 一致。
源格式或 FPS 改变须结束旧会话并重新建立连接。序号和时间必须严格递增；离线还要求序号连续。
接收端不把机器间时钟差解释为单向延迟，也不使用墙钟重写采集时间。

实时 ACK 是 8 字节 u64 LE 原始帧序号，在 payload 校验和可选录像写入后返回。
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
