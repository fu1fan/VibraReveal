# 10-env · 统一开发环境镜像

本目录提供一个镜像，同时供**云原生开发（WebIDE）**和**云原生构建**使用，内容包括：

| 能力 | 组成 | 位置 |
| --- | --- | --- |
| 运行仓库 Python 参考实现 | Python 3.10 + `numpy` / `opencv-python-headless` / `matplotlib` | 系统解释器 |
| 构建 ELF2 开发系统 | ELF2 构建主机依赖（与子模块 `.ci/Dockerfile` 一致）+ 固定工具链/rootfs/厂商材料 | `/opt/elf2-deps` |
| ELF2 交叉编译 | `aarch64-none-linux-gnu` 工具链、CMake 工具链文件、`elf2-sysroot` | `/opt/elf2-deps`、`/opt/elf2/cmake` |
| WebIDE | code-server + CNB 官方示例插件 + CodeBuddy 插件与 CLI | `/root/.local/share/code-server` |
| IDE 状态持久化 | `~/.cnb/<repo>/` 软链 + CNB 文件漫游 | `10-env/persist.sh` |

目录：

```text
10-env/
├── Dockerfile                        # 镜像定义
├── build-image.sh                    # 组装最小上下文并构建镜像
├── profile.d/elf2.sh                 # 交叉编译环境变量（登录 shell 自动加载）
├── cmake/elf2-aarch64-toolchain.cmake # CMake 交叉编译工具链文件
├── persist.sh                        # IDE/CodeBuddy 状态漫游软链
└── bin/
    ├── python                        # python 选择器（ELF2 构建用 2，日常用 3）
    ├── elf2-build                    # ELF2 系统增量构建
    ├── elf2-env                      # 环境自检报告
    └── elf2-sysroot                  # 从 rootfs 基线抽取目标 sysroot
```

## 1. 镜像发布

镜像**显式构建并推送**到 CNB Docker 制品库，不使用 CNB 对 Dockerfile 的自动缓存：

```text
${CNB_DOCKER_REGISTRY}/${CNB_REPO_SLUG_LOWERCASE}/dev-env:latest
${CNB_DOCKER_REGISTRY}/${CNB_REPO_SLUG_LOWERCASE}/dev-env:<tag>
```

- **触发条件只有一种：推送 `dev-env-v*` 形式的 Tag**。普通分支提交不会重建镜像，
  即使改动落在 `10-env/**` 也一样，避免为每次小改重跑一次含 2.7 GB 依赖下载的全量构建。

  ```bash
  git tag dev-env-v1.0.0
  git push origin dev-env-v1.0.0
  ```

- 同一时刻只允许一个发布任务（`lock`）；该 Tag 也会按仓库约定同步到 GitHub。
- 镜像标签为 Tag 名（`.../dev-env:dev-env-v1.0.0`）与 `latest`，`latest` 指向最近一次成功发布。
  云原生开发引用 `latest`；要固定版本或复现旧环境，把 `$: vscode:` 的 `image` 换成具体 Tag。
- 镜像发布成功之前，云原生开发会因为拉不到镜像而起不来（`docker.image` 没有 `build` 兜底），
  所以顺序是：打 Tag → 等发布流水线成功 → 再启动/重建开发环境。

本地构建（需要已初始化子模块）：

```bash
git submodule update --init 02-platform/RK3588-OV9281
IMAGE=vibrareveal-dev-env:local bash 10-env/build-image.sh
```

`build-image.sh` 只把 Dockerfile 需要的几个文件复制进临时上下文，避免把 2.4 GB 子模块
和硬件资料送进 Docker。

镜像约 6~8 GB，主要来自 `/opt/elf2-deps` 中按 SHA256 固定的依赖（压缩后 2.7 GB）。

预装插件（open-vsx 源）：`cnbcool.cnb-welcome`、`redhat.vscode-yaml`、
`dbaeumer.vscode-eslint`、`mhutchie.git-graph`（以上为 CNB 自定义开发环境官方示例中的全部插件）、
`ms-python.python`、`llvm-vs-code-extensions.vscode-clangd`、`mathematic.vscode-pdf`（PDF 查看器）、
`tencent-cloud.coding-copilot`（CodeBuddy）。同时安装 CodeBuddy Code CLI（`codebuddy` 命令）与
`clangd` / `clang-format`。
微软的 C/C++ 扩展不在 open-vsx 内，code-server 无法安装，因此 C/C++ 提示由 clangd 提供。

## 2. 云原生开发

`.cnb.yml` 的 `$: vscode:` 直接引用上面的镜像，并挂载四个数据卷：

| 卷 / 路径 | 作用 |
| --- | --- |
| `elf2-system-v1:/cache/elf2-work` | ELF2 SDK 工作区（保留 `.o`、依赖文件和生成头文件） |
| `elf2-system-v1:/cache/elf2-ccache` | ELF2 系统构建的 ccache |
| `/cache/elf2-app-ccache` | 交叉编译 elf2 应用程序的 ccache |
| `/root/.cache/pip` | pip 下载缓存 |

启动时执行 `10-env/persist.sh` 建立持久化软链；环境销毁前执行 `persist.sh --report`
打印漫游目录占用。

云原生构建复用同一镜像，在任意流水线里写：

```yaml
docker:
  image: ${CNB_DOCKER_REGISTRY}/${CNB_REPO_SLUG_LOWERCASE}/dev-env:latest
stages:
  - name: build
    script: elf2-env
```

仓库里现成的例子是 `.cnb.yml` 的 `web_trigger_env_check` 手动入口。

### 与流水线共享增量缓存

ELF2 系统流水线在子模块仓库 `fu1fan/RK3588-OV9281` 中运行，使用同样的卷组名和路径
（`elf2-system-v1:/cache/elf2-work`、`elf2-system-v1:/cache/elf2-ccache`）。能否真正共享取决于：

- CNB 数据卷只在**同一构建节点**内有效，跨节点不复用；
- 卷是否在同一仓库命名空间下（流水线运行在子模块仓库）。

镜像中 `/etc/elf2-ci-packages.txt` 刻意在与流水线镜像相同的软件集合下生成，使
`.ci/build.py` 计算出的工作区兼容键一致，因此**只要调度到同一节点**，缓存键就能对上。
这是尽力而为，不保证命中；不命中时只是冷编译一次。

## 3. 运行仓库 Python 代码

```bash
cd 04-algorithms/phase-motion
python3 -X utf8 phasemag.py --selftest --outdir /tmp/selftest_out
```

依赖已预装在系统 Python 3 中，无需创建虚拟环境；需要隔离时仍可按 README 建 `.venv`。

`python` 是一个选择器：ELF2 厂商 SDK 的 U-Boot 脚本通过 `python` 调 Python 2，
`.ci/build.py` 拉起厂商脚本时会设置 `USER=elf2-ci`，此时 `python` 指向 Python 2；
其余情况指向 Python 3。手工执行厂商构建脚本需要 Python 2 时用 `ELF2_PYTHON=2 ./build.sh ...`。

## 4. ELF2 系统构建

```bash
cd 02-platform/RK3588-OV9281
elf2-build                      # 增量构建，成功后自动提交缓存标记
ELF2_BUILD_MODE=clean elf2-build # 干净构建
elf2-build --finish             # 只补提交缓存标记
elf2-env                        # 查看工具链、依赖、缓存目录与缓存键
```

- 固定依赖来自镜像 `/opt/elf2-deps`，不需要重新下载 2.7 GB 附件。
- 工作区在 `/cache/elf2-work/<缓存键>/sdk`，源码按当前子模块 HEAD 同步，删除和切换提交都会正确处理。
- ccache 在 `/cache/elf2-ccache`，上限 10 GB。
- **构建成功后必须提交缓存标记**：没有标记的工作区会在下次构建开始时被判定为中断并整目录丢弃。
  `elf2-build` 已自动处理；只有直接调用 `python3 .ci/build.py` 时才需要手动 `--finish`。
- 产物在 `02-platform/RK3588-OV9281/dist/`。构建过程会校验 rootfs、模块和 `update.img`，
  但**不代表已通过开发板验收**。
- 需要至少 15 GiB 空闲空间存放工作区。

开发环境的数据卷用 `read-write`：会话中的写入立即落盘，不依赖 `copy-on-write`
「流水线成功后才合并」的语义，因此下次登录可以继续增量编译。代价是若同一节点上
同时运行流水线并写同一卷组，需要自行错开（流水线本身有 `lock`，开发环境不会自动排队）。

## 5. 交叉编译 elf2 程序

登录 shell 会自动加载 `/etc/profile.d/elf2.sh`：

| 变量 | 含义 |
| --- | --- |
| `ELF2_CROSS_COMPILE` | 工具链前缀，例如 `/opt/elf2-deps/prebuilts/gcc/linux-x86/aarch64/<ver>/bin/aarch64-none-linux-gnu-` |
| `ELF2_SYSROOT` | 工具链 sysroot |
| `ELF2_WORK_CACHE` / `ELF2_CCACHE_DIR` | 系统构建的工作区与 ccache |
| `CCACHE_DIR` | 应用交叉编译的 ccache |

Makefile 项目：

```bash
make CROSS_COMPILE="$ELF2_CROSS_COMPILE" ARCH=arm64
```

CMake 项目：

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/opt/elf2/cmake/elf2-aarch64-toolchain.cmake
cmake --build build
```

需要 elf2 上的额外库（例如 Vulkan/Mali 运行时）时，先展开目标 sysroot，再用它作 `--sysroot`：

```bash
elf2-sysroot                                     # 展开到 /cache/elf2-sysroot（一次性，约 1~2 GB）
cmake -B build -DCMAKE_TOOLCHAIN_FILE=/opt/elf2/cmake/elf2-aarch64-toolchain.cmake
# 展开后 shell 里重新加载 profile 或直接指定
ELF2_SYSROOT=/cache/elf2-sysroot cmake -B build -DCMAKE_TOOLCHAIN_FILE=/opt/elf2/cmake/elf2-aarch64-toolchain.cmake
```

工具链 sysroot 只提供 libc 等基础库；业务库请以展开后的 rootfs 为准。

## 6. IDE 与 CodeBuddy 状态持久化

CNB 的文件漫游按用户维度保存 `~` 下的配置，环境销毁时写入、重建时恢复。镜像内的
`persist.sh` 在启动时把下列路径软链到 `/root/.cnb/<repo>/`（该目录本身会被漫游）：

| 目标 | 内容 |
| --- | --- |
| `~/.local/share/code-server/User/globalStorage/tencent-cloud.coding-copilot` | CodeBuddy 插件设置与状态（只挑这一个扩展目录，不整目录搬） |
| `~/.local/share/code-server/User/History` | 本地文件历史 |
| `~/.local/share/code-server/User/Machine/settings.json` | WebIDE 机器级设置 |
| `~/.vscode-server/data/Machine/settings.json` | Remote-SSH 机器级设置 |
| `~/.local/share/CodeBuddyExtension/Data` | CodeBuddy 扩展运行时数据（登录态、对话历史）；`Cache`、`Logs` 可重建，不搬 |
| `~/.agents` | CodeBuddy 技能实体（`~/.codebuddy/skills` 以软链引用） |
| `~/.codebuddy/*` | CodeBuddy Code CLI 的数据，按排除表筛选：排除 `logs`、`traces`、`debug`、`diagnostics`、`blobs`、`file-history`、`sessions`、`shell-snapshots`、`plugins` |

`~/.local/share/code-server/User/settings.json`、`keybindings.json`、`snippets/` 由 CNB 原生漫游，
脚本不介入。

几个实现细节：

- CodeBuddy Code 不同版本的目录结构不一致（例如出现了 `app/`、`local_storage/`、`expert-history.json`），
  所以 CLI 部分按**排除表**遍历实际条目，而不是写死清单；新增目录默认会被保留，除非命中排除表。
- `plugins/` 是 61 MB 量级的可重装内容，排除在外，避免挤占全账号共享的漫游配额。
- 常见配置文件（`settings.json`、`mcp.json`、`history.jsonl` 等）即使当前还没生成也会先建软链，
  保证之后由 CodeBuddy 创建的版本直接落进漫游目录；这类链在文件生成前是悬空的，属于预期状态。
- 目录被搬进漫游目录后，内部的**相对**软链会因层级变化失效（例如 `skills/* -> ../../.agents/skills/*`），
  `persist.sh` 会按原路径把它们改写成绝对路径。

限制与注意事项：

- 漫游只在环境**正常销毁**时发生，`kill 1` 关闭会丢失；同时多个环境时后销毁者覆盖先销毁者。
- 漫游总量约 100 MB（整个账号共享），单文件 100 MB。CodeBuddy 设置里有 `cleanupPeriodDays`（默认 30 天），
  会话积累过大时可先清理 `~/.codebuddy/projects/*/tool-results` 再退出。
- 想额外保留别的路径，可在仓库的「个人设置 → 云原生开发 → 漫游文件」里按绝对路径或 Glob 追加，
  不必改镜像。

## 7. 已知限制

- 镜像由 `dev-env-v*` Tag 触发构建，未在流水线中做运行时验收；`web_trigger_env_check` 手动入口负责依赖与工具链自检。
- 开发环境默认 8 核；系统构建可用 `runner.cpus` 或 `ELF2_JOBS` 调整。
- 卷不跨节点，切换节点即冷启动；跨节点中间文件迁移未启用。
- 镜像自带插件来自 open-vsx 扩展源；需要微软官方源独有的扩展时，按 CNB 文档下载 `.vsix` 后
  `code-server --install-extension xxx.vsix` 安装。
