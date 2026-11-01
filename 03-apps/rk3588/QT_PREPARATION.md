# 1024×600 Qt/QML 应用准备

此说明保留环境准备调查记录。2026-11-01 已补齐匹配 SDK、完成 CNB ARM 构建和原板运行，见 [板端验证记录](../../08-docs/validation/rk3588-touch-2026-11-01.md) 与 [TOUCH_APP.md](TOUCH_APP.md)。
用户确认板端为 Qt **5.15.10**；正式构建必须精确匹配。Ubuntu 5.15.3 只作一般宿主测试。
RK3588 必须另过 `sh 10-env/check-qt.sh sdk` 的 5.15.10 EXACT 检查，再做板端运行验收。

## 可复用接口

| 功能 | 当前入口 | 集成边界 |
| --- | --- | --- |
| 原始采集 | `Camera::read`, `Frame`（`src/frame.hpp`） | 带尺寸、stride、位深、sequence、timestamp；唯一采集者，复制后立即回收 V4L2 buffer |
| 峰值对焦/选中对象边缘 | `VulkanMotion::analyze`, `EdgeResult` | 独立工作线程；`analysisOnly=true`；掩膜与结果保留原图坐标/帧身份 |
| 对象选择 | `MobileSam::setImage/select` | 同帧 embedding 可多次修正；不自带跨帧追踪 |
| 局部实时放大 | `VulkanMotion::process` | 固定 ROI、连续采样；改变 ROI 重建实例；不能随意丢弃时间滤波输入 |
| 离线分析 | `processOffline` / `vr-offline` | 完整 VRI1，磁盘 scratch，不把 RAM 临时目录当无限磁盘 |
| RAW 电脑直通 | `vr-stream` bulk/TCP | 现有传输程序，不是本地录像控制器；TCP latest-frame 路径可能丢帧 |

准备阶段缺少的实时频谱现已在 `src/gui/measurement.cpp` 实现；其测量近似和适用边界见应用说明。
峰值对焦是清晰度辅助，不是自动对焦电机控制或物理位移标定。

## 后续 UI/服务约束

- 1024×600 触屏预览支持按比例缩放与拖动；屏幕坐标必须逆变换到传感器坐标。
  峰值对焦模式固定 1:1，仅裁剪/拖动；显示窗口须扣除工具栏实际可视面积。
- 本地模式提供对象→边缘选择、实时频谱/局部放大和本地 RAW 录像。
  电脑直通只保留固定峰值预览与状态，不开放本地对象/边缘、频谱和放大操作。
- 4GB 是 DDR 写缓冲目标，不等于设备可安全分配 4GB。启动时按可用内存、系统/GPU/NPU
  预留和帧容量确定有效预算并显示；内存分配失败必须拒绝开始。
- 独立录制状态机至少区分录制、停止采集、排空、已保存、失败。容量不足必须先停止接收
  新录像帧，再排空已接收帧、校验并完成文件发布；不能套用预览的丢旧帧队列。
  写满/断盘等错误保留 partial 和错误状态，不声称保存完成。该状态机现已在 `src/gui/recording.cpp` 实现。
- 采集只有一个 owner；预览允许丢旧任务，但录像/时域分析必须明确连续性策略。
  不为 QML 另外启动第二个相机程序。用 worker/queued signal 与 GUI 线程隔离 GPU/NPU/I/O。
- 本地录像完成后支持离线分析及传电脑；直通模式由电脑录制处理。模式切换必须先完成
  停采/排空或明确失败，不能直接销毁尚有待写帧的会话。

## 历史缺口调查（已于 2026-11-01 补齐）

### 5.15.10 目标 SDK 来源与缺口

系统子模块为 sparse checkout，配置可用 `git -C 02-platform/RK3588-OV9281 show HEAD:<path>`
读取，不能因工作树不可见就判定源码不存在。已核对：

- `buildroot/package/qt5/qt5.mk` 固定 5.15.10，源站为 `https://invent.kde.org/qt/qt`。
- qtbase 为 `e44097b63d17ba3178a637df7fac51ddc51cb48b`；qtdeclarative 为
  `b3aaf1482c48bbc0ca4f7c7934597c055afe4b6a`；qtquickcontrols2 为
  `74e8db97fdb9b55c4b4614b83f4bf17e00df55e9`。
- **全局版本与包源码不一致**：上述三个 commits 的 `.qmake.conf` 实际均写
  `MODULE_VERSION = 5.15.6`，qtbase 的 qmake bootstrap 也识别 5.15.6。因此不能把该包锁
  当作板端 5.15.10 的构建来源，不能通过改版本宏强行匹配。
- `elf2_fs_defconfig` 包含 `rockchip/qt.config`；qtbase 包启用 staging，配置
  `-prefix /usr -hostprefix $(HOST_DIR) -headerdir /usr/include/qt5 -sysroot $(STAGING_DIR)`。
- `rockchip/qt.config` 显式启用的是 Qt Quick Controls 1，未显式启用 Controls 2；
  不能据此保证预制 rootfs 的 `QtQuick.Controls 2.15` / Templates 2 插件存在，仍需检查实际镜像。
- `device/rockchip/common/scripts/mk-rootfs.sh` 优先直接复制预制 rootfs；当前 CI 每轮恢复
  该预制文件，因此不能假设一次系统构建就产生 Qt 的 host/staging 开发产物。
- `.ci/export_sdk.py` 只取工具链 sysroot 和最终 rootfs 的 lib/usr/lib/usr/include；
  没有单独导出 Qt host tools，也没有 Qt CMake/moc/rcc 交叉链接验收。

可实施补齐方案：先找回预制 rootfs **真实 5.15.10** 的源码/补丁/配置身份（当前包锁不足），
取得与预制 rootfs 同配置、同源码的 `buildroot/output/elf2_fs/staging`
及 `host`（或先只构建对应 Qt 开发包，不发布/刷写 rootfs），将目标头、未剥离开发链接、
CMake packages、mkspecs 合入 SDK sysroot，将 amd64 moc/rcc/qmake/qmlcachegen 及依赖放
独立 `host-qt`。必须验证目标 Qt 版本、配置和库身份与现板一致；仅相同版本号仍不足够。
重写绝对路径和 qt.conf，确保 Qt5::moc/rcc 指向宿主工具，库/头只来自目标 sysroot。
搬迁到另一目录后做 `find_package(Qt5 5.15.10 EXACT)`、AUTOMOC/AUTORCC、QML/Quick/
Controls2 链接及 AArch64 ELF 检查；通过后才可发布新 SDK digest，板端 Qt 不变。
上述为找到原厂完整 SDK 之前的状态；最新来源、身份匹配和搬迁构建结果见文首验证记录。

固定 SDK `d2a21493198e25881cb03144bf4dd45b76e773e6cbbad1b62d6debbd4aa73cef`
已在本机只读检查：manifest 的 system_commit 与锁文件一致；包含 Qt Core/Quick/
QuickControls2/QuickTemplates2 的 `5.15.10` 运行库和 `.so` 链接。未找到 Qt5/Qt5Core
CMake package、`qconfig.h`、moc/rcc/qmake 或 `rknn_api.h`，故现产物不够直接编译 Qt 应用。
SDK 内未找到 `QtQuick/Controls.2`、`QtQuick/Templates.2` 的 qmldir；这是 SDK 导出内容的
限制，不能直接推断板端原始 rootfs 缺少这些模块。后续需查原始 QML 安装路径并补全导出范围。
Qt5Core 文件实测为 AArch64 ELF，其内置构建字符串为
`Qt 5.15.10 (arm64-little_endian-lp64 shared (dynamic) release build; by GCC 12.3.0)`。

2026-11-01 在 CNB 对固定校验值的基础 rootfs 执行只读检查：实际 QML 路径是
`/usr/qml`，其中有 `QtQuick/Controls.2` 和 `QtQuick/Templates.2` 插件；平台插件位于
`/usr/lib/qt/plugins/platforms`，包含 EGLFS。原 SDK 导出仅包含 lib 与 include，
所以遗漏 `/usr/qml`，但源码头文件、目标 CMake 配置和对应宿主工具仍未找到。
这是对基础镜像的检查，不证明当前板端文件未被修改。证据：
[`cnb-kjs-1k6ah1efj`](https://cnb.cool/fu1fan/RK3588-OV9281/-/build/logs/cnb-kjs-1k6ah1efj)。
该次调度节点上的系统缓存未发现 Qt staging：
[`cnb-r2h-1k6agsk31`](https://cnb.cool/fu1fan/RK3588-OV9281/-/build/logs/cnb-r2h-1k6agsk31)；
不能据此推断其他节点的缓存状态。

同日实际板端只读核对：Qt Core 和 Quick 两个 5.15.10 库的 SHA-256 与固定 SDK
逐文件相同，分别为 `e79d0acbf2fe2f7c3e6993aacdbef71c13b293bd2d284c440daa0c8a1e272a94`、
`c20f1c4184f4a6dccdb7508c7c30c80e5819d9f3e8ec8c2f19551be314a33984`。
板端 `/usr/qml/QtQuick/Controls.2`、`Templates.2` 和 EGLFS/Wayland 插件均存在；
在根文件系统与 `/userdata` 未找到 qmake、qconfig.h 或 Qt5CoreConfig.cmake。
系统运行 Weston，其 socket 为 `/run/wayland-0`。

CNB 原厂二进制资产包已完成 SHA-256 检验和文件名检查，未发现 Qt staging、qmake 或
Qt 开发工具，见 [`cnb-vha-1k6ah4ugk`](https://cnb.cool/fu1fan/RK3588-OV9281/-/build/logs/cnb-vha-1k6ah4ugk)。
仅检查此固定资产包，不代表其他原厂资料或原始开发环境中不存在开发文件。

目标 SDK Qt 的头/库/CMake package、匹配版本的宿主 moc/rcc、搬迁路径和链接；板端 Qt
Quick 平台插件、触摸、GPU 显示与 Vulkan 计算并发；真实相机/NPU/磁盘/USB 负载和热稳定性。
宿主离屏冒烟不覆盖这些项目，不构成目标部署就绪证明。

2026-10-31 初期开发准备验证（历史记录）：独立 Ubuntu 22.04/amd64 容器安装上述依赖后，Qt 5.15.3
的 C++/moc 编译、链接和 1024×600 QML 控件实例化通过（`QT_HOST_QML_VERIFIED`）。
本机环境回归 13 项中 10 项通过、3 项因 Linux 专用工具跳过；shell 语法和 diff 检查通过。
完整 daily-env 镜像未重建/发布，目标 SDK 的 Qt 交叉链接检查及板端运行未执行。

后续已改用官方源码构建的 Qt 5.15.10 宿主环境，应用构建及 5 项 CTest 全部通过；
当前结果见 [触屏应用验证记录](../../08-docs/validation/rk3588-touch-2026-10-31.md)。
