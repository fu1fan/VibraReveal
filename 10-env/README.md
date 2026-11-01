# 日常开发、按需 SDK 与系统构建

日常环境负责 IDE、Python 算法和应用开发；完整系统环境负责内核、rootfs 和固件构建。
系统流水线发布的应用 SDK 按 digest 加载，不把几 GB 的厂商材料和 rootfs 放进日常镜像。

## 快速开始

打开 CNB 云原生开发即可使用 Python、code-server、CodeBuddy、clangd、CMake。
启动不拉取 RK3588 子模块，不下载应用 SDK，不执行系统构建。

第一次交叉编译：

```sh
elf2-sdk use                       # 按 10-env/sdk.lock.json 安装 SDK，可提前预热
elf2-cmake -S my-app -B build/elf2  # 同样会自动安装 SDK；默认使用固定版本
elf2-sdk run cmake --build build/elf2
elf2-sdk run sh -c '"${ELF2_CROSS_COMPILE}gcc" --sysroot="$ELF2_SYSROOT" main.c -o app'
```

`elf2-sdk use` 不修改父 shell，也不切换全局版本。`run` 为子进程设置工具链、sysroot、
PATH 和隔离后的 pkg-config 搜索路径。需要交互式交叉编译 shell 时执行 `elf2-sdk run bash`。
更换 SDK 后使用新的 CMake build 目录，避免旧编译器路径被 CMake 缓存。

默认锁文件为 `/workspace/10-env/sdk.lock.json`，其他项目可指定：

```sh
elf2-sdk --lock /path/to/sdk.lock.json run cmake ...
```

锁文件必须同时记录产物镜像 digest 和系统提交：

```json
{
  "image": "docker.cnb.cool/fu1fan/rk3588-ov9281/app-sdk@sha256:<64位摘要>",
  "system_commit": "<对应系统完整提交SHA>"
}
```

不能使用 latest 或其他可变标签。更新前确认开发板系统版本；编译通过不等于已经完成板端 ABI 验收。

## SDK 的来源和缓存

RK3588 系统流水线在验证最终系统后导出 `/sdk`：工具链、开发 sysroot、最终 rootfs 的
库和头文件、系统身份与校验清单。导出后在另一个路径编译链接 C、C++、Vulkan 样例。
固件 update.img、内核配置和 Module.symvers 仍通过系统仓库的原有附件发布流程获取。
应用 SDK 不承诺包含全部第三方开发头文件，也不是完整的内核模块开发树。

首次使用由 `docker pull` 拉取固定 digest，创建不运行的容器，并用 `docker cp` 提取 `/sdk`。
安装使用文件锁、临时目录和原子重命名；校验系统提交后才写完成标记。失败不发布部分缓存。
缓存命中后无需访问 Docker 或网络。同一节点的新环境通过 `elf2-sdk-v1:/cache/elf2-sdk`
的 copy-on-write 数据卷复用：流水线成功或开发环境正常成功结束后合并缓存；失败、强杀
或异常退出可能丢失本轮新增缓存，需要重新下载。不同会话写入隔离，同一会话安装仍有文件锁。
此选择已在 CNB 实测可写；read-write 卷在当前 UID 映射下出现 65534 所有权与写入拒绝。
不同 digest 并存，避免项目之间覆盖。冷节点首次编译仍需下载 SDK，只是不会阻塞打开 IDE。

Docker 镜像层与提取缓存可能各占一份空间；本工具不自动删除 Docker 镜像，以免干扰其他使用者。
SDK 缓存也不自动清理。确认没有构建使用后，可手动删除不再需要的 digest 目录。

## Qt/QML 开发准备

日常镜像通过独立构建阶段，从官方源码及固定 SHA256 编译宿主 Qt **5.15.10**，只将
`/opt/qt-5.15.10` 安装产物复制到最终镜像，提供 Qt Quick/Controls 2/QML/Test/QuickTest、
Ninja 和 glslangValidator。默认 PATH/CMake/QML 插件路径均指向该版本。
镜像构建时编译含 moc 的最小 C++ 程序，
以 offscreen/software 模式实例化 1024×600 QML 窗口及控件，无需显示器或 GPU。
这只是宿主开发及 QML 模块检查，不验证渲染、触摸、性能或目标 ABI。

```sh
sh 10-env/check-qt.sh host  # 宿主编译和 QML 实例化
sh 10-env/check-qt.sh sdk   # 固定 SDK 的 Qt 5.15.10 EXACT 交叉链接；不运行目标程序
```

第二条必须使用 SDK 自己的目标 Qt 头文件、库、CMake package，以及版本匹配且能在
amd64 构建机运行的 moc 等工具。安装宿主 Qt **不会**给 RK3588 sysroot
补齐 Qt。2026-11-01 锁定的 `qt-app-sdk` 已加入匹配的原厂 Qt 开发文件并通过 CMake/qmake 搬迁链接及原板运行；旧系统 SDK 仅保证 C/C++/Vulkan。
`sdk` 检查失败时应在系统仓库补齐配套 Qt staging 开发文件和宿主工具、做搬迁检查，
发布新 SDK 并更新 digest/system_commit，禁止用宿主库绕过检查。
还须确认板端 Qt 版本、QML 插件、平台插件、触摸输入与图形驱动兼容；现阶段不升级
板端 Qt 或刷系统。应用接口准备见 [Qt/QML 准备](../03-apps/rk3588/QT_PREPARATION.md)。

用户已确认板端 Qt 为 **5.15.10**，系统仓库 `buildroot/package/qt5/qt5.mk` 亦固定该版本。
Ubuntu 5.15.3 仅为一般宿主测试环境，不能用于正式 Qt 5.15.10 构建验收。
`build-qt51510-host.sh` 用官方 Qt 5.15.10 发布源码及 SHA256 构建独立的软件离屏宿主 Qt；
它仍不是 AArch64 目标 SDK，也不证明与板端平台插件/图形配置一致。
`elf2-sdk run` 会清除原生 Qt 的 CMake/QML/plugin 查找变量，并移除其 PATH/动态库路径；
交叉 CMake 的库/头/package 查找仍仅限 sysroot。配套宿主工具须通过目标 SDK 的配置
显式绑定，不能把原生 Qt 的 CMake package 当作 ARM 依赖。
宿主与 ARM 构建必须使用不同的 build 目录；环境清理不会修复已有 CMake 缓存中的宿主路径。

本地已经保留 `vibrareveal-qt51510-tests:local`：官方 5.15.10 源码构建，精确版本
CMake 检查、C++/moc、QML 实例化和 QuickTest 控件点击均通过；含 clang-format 15。
默认 offscreen/software，字体路径为 `/usr/share/fonts/opentype/noto`，Vulkan 默认 lavapipe。

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/workspace" -v vr-gui-build:/build \
  vibrareveal-qt51510-tests:local sh
# 容器内：cmake -S 03-apps/rk3588 -B /build -G Ninja ...
# Qt 路径已设置为 /opt/qt-5.15.10；按应用 CMake 选项开启 GUI 和后端。
```

完整 CNB daily 镜像仅更新了上述构建配置，本轮没有重建或发布。宿主 Qt 使用软件
离屏配置、无 OpenGL/XCB，适用于测试和生成工具；它不是板端显示/触摸验收环境。

### 本地 Linux Qt 测试镜像

独立的 `Dockerfile.qt-test` 不安装 IDE，也不包含目标 SDK。构建一次后可复用镜像层：

```sh
docker build --platform linux/amd64 -f 10-env/Dockerfile.qt-test \
  -t vibrareveal-qt-tests:local 10-env
docker run --rm --platform linux/amd64 \
  -v "$PWD:/workspace" -v vr-qt-build:/build \
  vibrareveal-qt-tests:local sh -c \
  'cmake -S 03-apps/rk3588 -B /build -G Ninja -DVR_BUILD_VULKAN_MOTION=ON && cmake --build /build -j4'
docker run --rm --platform linux/amd64 \
  -v "$PWD:/workspace" -v vr-qt-build:/build \
  -e VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  vibrareveal-qt-tests:local xvfb-run -a ctest --test-dir /build --output-on-failure
```

镜像包含 Qt 5.15 Core/Quick/Controls2/Test/QuickTest、CMake/Ninja、Vulkan 开发库、
Mesa lavapipe 软件 ICD、glslang、Xvfb、DejaVu/Noto CJK 字体和 Python NumPy/Pillow。
默认 Qt Quick 使用软件场景图；Vulkan 计算仍经软件 ICD 执行。构建时验证 QML 控件
实例化、Qt Quick Test 鼠标交互和 Vulkan 设备枚举。容器测试不证明 Mali/NPU 或触屏实机性能。
RKNN 头需单独只读挂载并设置 `RKNN_INCLUDE_DIR`；镜像不附带 NPU 运行时/模型。

## 完整系统构建

```sh
elf2-system
# 首次会初始化锁定的 RK3588 子模块、拉取 system.lock.json 固定的完整镜像。
# 在完整环境 shell 中：
elf2-build
# 产物回到 /workspace/02-platform/RK3588-OV9281/dist
```

`elf2-system` 使用 CNB Docker 服务并挂载工作区，适用于终端交互。完整环境镜像固定在
`system.lock.json`；当前可继续使用上一轮已验证的完整 dev-env 镜像，不需等待重新打包。
工具只按父仓库固定的子模块提交初始化，不自动追踪子模块 main。

完整环境保留 Python 2/3 选择器、厂商材料、基础 rootfs、elf2-build 和 elf2-sysroot。
elf2-build 对缓存与 SDK 产物目录加锁，忙时拒绝并发；不接受独立 --finish/--no-finish。
构建只使用提交后的 SDK 源码，未提交修改会报错。构建结果并不代表开发板验收通过。

嵌套系统容器的命名卷由 Docker 服务管理，其跨环境生命周期不作保证；正式系统发布和
持续增量构建应使用 RK3588 仓库的系统流水线及其 CNB 缓存卷。
`elf2-sysroot` 仍仅提取基础 rootfs，应用开发应优先使用本轮系统产出的版本化 SDK。

## 镜像构建与发布

- `Dockerfile`：日常镜像 `daily-env`，无需子模块；系统重依赖不在其中。
- `Dockerfile.system`：完整系统环境；仅在需要更新系统构建环境时构建。
- `build-image.sh`：组装最小上下文，避免传入整个 SDK 和硬件资料。

```sh
IMAGE=vibrareveal-daily:local bash 10-env/build-image.sh
git submodule update --init 02-platform/RK3588-OV9281
MODE=system IMAGE=vibrareveal-system:local bash 10-env/build-image.sh
```

页面“发布开发环境镜像”和 `api_trigger_publish_env` 发布日常镜像；`dev-env-v*` Tag
也发布日常镜像。`api_trigger_publish_system_env` 发布带构建编号的完整镜像，成功后按输出
的 digest 更新 system.lock.json。两个发布入口使用同一发布锁。

安装步骤保留有限重试，失败不会默默跳过扩展。镜像构建运行冒烟检查与脚本回归测试。
系统固定依赖独立于日常镜像，日常重建不再下载这部分材料。

## 持久化和启动速度

日常挂载 `/cache/elf2-sdk`、`/cache/elf2-app-ccache`、`/root/.cache/pip`，均使用 copy-on-write。
仍保留 Docker 服务供按需提取 SDK 和启动系统环境，其启动有少量开销。
不在启动阶段执行 elf2-sdk use，避免把 SDK 下载重新放回启动关键路径。
具体提速以 CNB 冷节点实测为准，不能保证跨节点镜像或文件缓存命中。

`persist.sh`/`persist.py` 在 IDE 启动时关联 CodeBuddy、History、机器设置与 ~/.agents 到
`~/.cnb/<repo>`。复制失败保留源数据；冲突时漫游数据优先，原数据保留在原目录旁
的 `.pre-roam-*` 备份中；支持动态及隐藏条目恢复。备份不在脚本的漫游目录内。
外部软链保留，迁移内部相对软链转换为绝对路径。迁移应在 IDE 写入前运行。

CNB 漫游依赖正常销毁环境，容量和多环境覆盖限制参见
[文件漫游文档](https://docs.cnb.cool/zh/workspaces/file-keeper.md)。
数据卷和服务配置依据 [CNB 语法手册](https://docs.cnb.cool/zh/build/grammar.md)。

## 验证

```sh
python3 -m unittest discover -s 10-env/tests -v
```

Linux 回归覆盖持久化故障、构建互斥、sysroot 提取、网络重试、SDK 校验与失败安装恢复。
真实 SDK 的编译链接在系统导出阶段验证，日常镜像发布后还应使用固定 SDK 做端到端自检。

### 2026-10-31 发布验证记录

- 原完整镜像层大小 4,638,323,745 字节；日常镜像 `daily-sdk-20261031-final2`
  为 848,640,822 字节，减少约 81.7%（CNB 制品库记录的镜像大小）。
- 日常镜像构建 `cnb-4v8-1k66vcj3b` 成功，13 项回归与镜像冒烟检查通过。
- 系统构建与 SDK 发布 `cnb-5sg-1k66uobrf` 成功，SDK 的 C/C++/Vulkan 搬迁编译验证通过。
- 消费端自检 `cnb-ojf-1k66vgrvu` 的 CMake Vulkan 编译、Docker 不可达时的缓存复用、
  完整系统容器工作区访问均通过。首次 SDK 拉取/提取及编译约 61 秒，该步骤不在 IDE 启动路径。
- 最终日常镜像自检 `cnb-3cn-1k66vpjd3` 全部成功，其中冷拉取日志为 14.8 秒、Prepare 为 25.8 秒；
  此为构建容器自检，不含完整 WebIDE 会话启动，不可直接当作 IDE 启动耗时。
- 未启动额外 WebIDE 会话测量完整冷启动，以免并行会话的漫游回写影响正在使用的环境。
  镜像体积下降不等于启动时间按相同比例下降；真实交互启动耗时待重建环境后确认。

## 重建板端 Qt 5.15.10 SDK

当前 `sdk.lock.json` 指向 `qt-app-sdk`，使用原系统 SDK 的编译器和目标运行库，仅补齐开发文件。
来源是原厂 `aarch64-buildroot-linux-gnu_sdk-buildroot.tar.gz`，SHA-256 为
`4a5c3e6deba91a6556917efab471dbffe6beb6c5b6cbb01ec1ffdb3f07b88ea8`。

在 Linux/amd64 解包原厂 archive，准备旧 SDK `d2a21493198e25881cb03144bf4dd45b76e773e6cbbad1b62d6debbd4aa73cef` 的 `/sdk`：

```sh
python3 10-env/augment-qt-sdk.py \
  --base /path/to/base-sdk \
  --vendor /path/to/aarch64-buildroot-linux-gnu_sdk-buildroot \
  --archive /path/to/aarch64-buildroot-linux-gnu_sdk-buildroot.tar.gz \
  --output /tmp/qt-sdk-image/sdk \
  --rknn-header /path/to/rknn_api.h
docker build -f 10-env/Dockerfile.qt-sdk -t qt-app-sdk:local /tmp/qt-sdk-image
```

脚本拒绝错误 archive 哈希或已有输出目录，并逐个核对 Core/Gui/Qml/Quick/Controls2/Templates2 的全部 ELF 可加载节。原厂库虽含未剥离信息，其运行内容与板端库一致；补齐后保留原目标 Qt 库字节。Qt 头、CMake、QML、mkspecs 和宿主工具分别加入 sysroot/qt-host，原厂绝对路径改为相对定位。qmake 配套 Mali 注入静态开发库一并保留。

交叉工具链显式优先链接目标 sysroot 的 libstdc++，满足板端 Qt/ICU 的 GLIBCXX_3.4.30；不会把编译器附带的旧运行库部署到板端。仓库内 `check-qt.sh sdk` 和 `build-touch-app.sh` 使用当前仓库的工具链文件，兼容尚未重建的日常 IDE 镜像。新容器若缺 `ninja` / `glslangValidator`，安装 `ninja-build glslang-tools`。

验证和具体边界见 [2026-11-01 板端记录](../08-docs/validation/rk3588-touch-2026-11-01.md)。
