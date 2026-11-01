# RK3588 触屏测量应用

`vr-touch` 将现有采集、Vulkan、MobileSAM、离线处理和 VRI1 传输接入一个 Qt/QML 应用。
目标屏幕为 1024×600；主图为相机预览，工具以不透明高对比浮层呈现，主要触控目标至少 48 px。
正式依赖为 **Qt 5.15.10**。CMake 同时检查 Qt 库和可运行的宿主 moc/rcc，启动时再次检查运行库版本。
2026-11-01 已完成 CNB ARM 构建、板端 Qt/Wayland/Vulkan、真实相机/USB 冒烟及主程序 NPU 对象选择验证，详见 [板端记录](../../08-docs/validation/rk3588-touch-2026-11-01.md)。人工触摸和长时间压力验收仍独立列项。

## 构建

必须使用与现板匹配的 ARM64 SDK，包含 Qt 5.15.10 的开发头文件、库、CMake 配置、
匹配的宿主工具，以及 Qt Quick、Quick Controls 2 和 Templates 2 插件。当前固定 SDK 是否具备这些内容，
以 `10-env/check-qt.sh sdk` 和应用构建结果为准。不要把 Ubuntu 的 Qt 路径加入目标链接搜索路径。

```sh
sh 10-env/build-touch-app.sh /tmp/vr-touch-build \
  -DVR_BUILD_RKNN_SELECTION=ON
```

主应用自动启用 Vulkan 后端，需要构建主机的 `glslangValidator`。不启用 RKNN 时，应用明确显示
“矩形区域”，仅提供手工选区；它不会声称完成 NPU 对象识别。NPU 模型和 runtime 的已验证字节身份见
[模型清单](models/validated-manifest.json)，不会随应用自动覆盖系统 `librknnrt.so`。

构建产物包括 `vr-touch`、`motion.comp.spv`、启动脚本和 `touch-build-info.json`。
构建脚本检查 ARM64 产物并记录 SDK digest、源码版本/脏状态和二进制哈希，不部署系统镜像。
`build-touch-app.sh` 默认启用 RKNN 后端；不需要 NPU 的构建可显式传入 `-DVR_BUILD_RKNN_SELECTION=OFF`。

## 运行

停止占用相机的旧服务，确认传感器真实帧率和非 compact RAW 格式（步骤见主 README），然后运行：

```sh
/etc/init.d/S91vrstream stop
/usr/local/sbin/vr-touch-launch \
  --device /dev/video11 --sensor /dev/v4l-subdev2 --width 1280 --height 800 --bits 10 \
  --shader /usr/local/share/vibrareveal-touch/motion.comp.spv \
  --storage /root/vibrareveal/recordings \
  --encoder /path/to/mobilesam_encoder.rknn --decoder /path/to/mobilesam_decoder.rknn
```

启动脚本优先使用现有 Wayland socket（缺省检查 `/run/wayland-0`），否则使用 `eglfs`；
可显式设置 `QT_QPA_PLATFORM` 覆盖。实际开发板已运行 Weston 并提供 Wayland 插件，
其他配置仍需确认对应插件安装。保留开发板原有 Qt/Mali 插件配置，不能混入宿主插件。
相机采集始终使用一个 owner；进入直通模式不会另起 `vr-stream` 争抢 V4L2。

### NPU 物品识别

采用 MobileSAM 的点/框提示分割：轻触物品或框住物品，识别该对象的区域，再确认并选择其测量边缘；不输出类别名称，也不做持续目标跟踪。模型在工作线程延迟初始化，重复修改同一参考帧的提示会复用编码结果。

生产程序自动发现 `/usr/local/share/vibrareveal-touch/models/mobilesam/` 下的 `mobilesam_encoder.rknn` 与 `mobilesam_decoder.rknn`。可用 `--models-dir` 指定其他目录，或显式提供 `--encoder` 和 `--decoder`。`--demo` 默认不自动启用硬件模型。

应用优先从 `/usr/local/lib/vibrareveal-touch/librknnrt.so` 加载配套运行库，也可通过 `--rknn-runtime` 指定；未安装应用专用运行库时按系统库搜索规则加载。不会修改全局 `LD_LIBRARY_PATH` 或覆盖系统 `librknnrt.so`。安装模型和运行库前须对照 `models/validated-manifest.json` 校验 SHA-256；本次已安装的模型为 Toolkit 2.5.0、RK3588 FP16、448 px encoder、8 个提示槽。

进入“对象选择”后，已配置模型时默认选中“NPU 识别”；识别中显示文字反馈，完成后叠加半透明对象区域与清晰边缘。可显式切换“手动框选”，模型缺失或推理失败不会冒充识别成功。关闭面板或切换方式会丢弃未完成请求的过期结果；设置页显示模型是否已配置，实际可运行性以首次识别结果为准。

`--manage-usb` 在进入电脑直通/发送文件时管理本应用的 `ffs.vri`：获取既有 USB 会话锁、
建立 FunctionFS、提交描述符、绑定 gadget；结束后移除自有功能并恢复原绑定。
该操作会重新枚举复合 USB，因此部署、终端管理应走 Wi-Fi。
不带此选项时仅使用外部已准备的 FunctionFS mount。会话超时后可在本机重新等待电脑。

TCP 是显式兼容通道，默认仅监听 loopback；绑定 USB NCM 地址时由启动参数指定：

```sh
vr-touch --transport tcp --bind 192.168.77.1 --port 5910 [其他采集参数]
# 电脑使用现有 vr-receive 或 vibrareveal stream 接收 VRI1。
```

TCP 和 Bulk 都使用 VRI1 固定尺寸流水标志，不逐帧 ACK，不降分辨率，不覆盖待传帧。
队列超限、采集缺口或连接失败会显示不完整会话。最终收到 `VRDONE01` 后才显示“电脑已确认完整接收”。
该确认表示协议接收完成，接收端最终文件发布仍应以电脑程序状态为准。

## 操作与实际处理

- **显示**：适应比例或 1:1；1:1 可拖动。对焦/电脑直通强制 1:1，退出后恢复此前比例。
- **对焦**：独立的全分辨率高斯/Sobel/非极大值抑制预览路径，保留 RAW 精度及原 Vulkan 分析阈值约定；避免为每帧预览计算对象区域、法向和大块 GPU 回读。峰值图仅叠加在对应帧上、本机显示。预览按显示刷新率取样，RAW 录制和直通仍接收每个原始帧；对焦提示显示实际交付的新画面帧率，格式栏显示传感器帧率。
- **对象与边缘**：冻结一张带原始序号/时间的参考帧，点选或拖动框选。可移动选框、拖动角点；
  有模型时调用 MobileSAM，否则明确使用矩形区域。确认对象后，在真实检测到的边缘上选一段。
  选区保存原图坐标，不冒充跨帧目标追踪。
- **实时放大**：调用现有 Vulkan 相位算法，局部结果与频谱分别可隐藏。处理帧率必须是源帧率的
  整数分频；原始采集仍逐帧检查连续性。队列或计算超时会停测，不继续显示损坏的滤波历史。
- **频谱**：所选边缘附近、方向一致的梯度像素提供亮度恒常近似下的法向小位移信号；
  去均值、Hann 窗、256 点 FFT，单边幅值以原图像素表示。需要一整窗连续样本；缺口会清空历史。
  它适用于清晰边缘的微小运动，尚无物理单位标定；光照变化、遮挡和较大位移会破坏该近似。
  “相位增益”不是经过标定的物理位移倍率。降低处理帧率没有时间抗混叠，目标频率必须低于处理 Nyquist。
- **RAW 录制**：全幅、原位深/stride/序号/时间，不写入图形叠加。缓冲最大 4 GiB，启动时根据
  MemAvailable 扣除系统预留设置有效预算；在途写盘帧计入预算，额外保留两帧空间。
  容量/内存不足停止接收此录像的新帧，排空已接受帧；预览采集仍可继续。
- **保存**：每帧 CRC、完整 END、文件 fsync、无覆盖发布及目录 fsync 完成后才报告已保存。
  中途采集缺口保留有效连续前缀并记录原因；写盘/空间不足保留 `.partial`，不伪装成功。
  `.json` 附属说明记录停止原因、帧数、时长和有效缓冲预算。
- **离线**：文件列表仅显示具有完整结束标记的 `.vri`。回放帧校验 CRC；在该录像的参考帧重新选择
  对象和边缘，然后调用现有完整片段 zero-phase 离线处理，完整验证源录像后输出 `.vrv`。
  临时相位数据放在所选存储目录而非无界 DDR；完成后显示末尾 FFT 窗口频谱。
- **传电脑**：已保存的 `.vri` 逐帧验证后传输，原始文件保留。当前不支持断点续传；中断后重新发送。
  录制时禁止历史文件传输，避免争用 eMMC；直通时本机只保留峰值预览和传输状态。

## 线程和状态

采集线程立即复制并回收 DMA，分发共享不可变帧给录制/传输/计算。
录制写盘、电脑传输、GPU/NPU/离线处理各有独立工作线程；GUI 以有限频率取得最新显示快照。
预览可以跳过旧任务，时域处理只接受有界的连续队列。切换工具/参数有 generation 检查，迟到结果不覆盖新状态。

录像状态为 `idle → recording → draining → saved/failed`。进入电脑直通先停止并等待保存；
低内存、缺口、写失败分别保留原因。退出时正常排空，无法完成的文件保持 `.partial`。
操作系统不可中断的磁盘 I/O 和驱动调用仍可能延迟退出，不能据此保证硬实时停机。

## 验证

不带 Qt 的核心测试：

```sh
cmake -S 03-apps/rk3588 -B /tmp/vr-core -DVR_BUILD_TOUCH_CORE_TESTS=ON
cmake --build /tmp/vr-core --parallel 4
ctest --test-dir /tmp/vr-core --output-on-failure
```

在 Qt 5.15.10 **宿主**环境中（与目标交叉构建分开）：

```sh
cmake -S 03-apps/rk3588 -B /tmp/vr-touch-host -G Ninja \
  -DVR_BUILD_TOUCH_UI=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/vr-touch-host --parallel 4
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  ctest --test-dir /tmp/vr-touch-host --output-on-failure
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
  /tmp/vr-touch-host/vr-touch --demo --windowed --transport tcp \
  --shader /tmp/vr-touch-host/motion.comp.spv --storage /tmp/vr-recordings \
  --exit-after 5 --screenshot /tmp/vr-touch.png
```

合成源必须显式使用 `--demo`，界面持续标明合成相机，录像也设置合成时间标志。不会在真实相机失败时偷偷回退演示。

已通过板端 Qt 5.15.10/Controls2 ABI、Wayland 显示与 Vulkan 并发、真实相机 RAW 录制回放、USB 直通及 MobileSAM 主程序点选/框选/选边。仍待人工触摸、长时 120 fps、4 GiB 实机压力/磁盘满、反复 USB 断连、复杂场景识别质量、温升和真实位移标定验收。

原板 `/userdata` 仅约 1 GiB，示例使用根分区的 `/root/vibrareveal/recordings`；请按实际剩余容量选择 `--storage`。应用启动前停止旧采集服务，退出后可执行 `/etc/init.d/S91vrstream start` 恢复。

### 对象边缘与放大范围

NPU 分割区域保持蓝色半透明填充，对象边缘及选中的测量边缘统一为红色。选边成功后，程序围绕实际参与测量的边缘像素生成局部矩形，四周预留 16 像素，最小 64x64 像素，并限制在原始图像内。红框及尺寸标签表示实际用于实时/离线视频放大的区域；确认后仍保留显示，适应、1:1 和拖动画面共用原图坐标映射。重新选边会更新范围，选边失败时清除旧范围的就绪状态，避免继续使用过期选区。
