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
缓存命中后无需访问 Docker 或网络，同一节点的新环境通过 `/cache/elf2-sdk` 数据卷复用。
不同 digest 并存，避免项目之间覆盖。冷节点首次编译仍需下载 SDK，只是不会阻塞打开 IDE。

Docker 镜像层与提取缓存可能各占一份空间；本工具不自动删除 Docker 镜像，以免干扰其他使用者。
SDK 缓存也不自动清理。确认没有构建使用后，可手动删除不再需要的 digest 目录。

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

日常挂载 `/cache/elf2-sdk`、`/cache/elf2-app-ccache`、`/root/.cache/pip`。
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
