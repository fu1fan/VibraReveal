# Native Qt tools only. Target Qt must still come from the pinned ARM SDK.
FROM ubuntu:22.04 AS qt-host-build
ARG DEBIAN_FRONTEND=noninteractive
RUN printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\n' > /etc/apt/apt.conf.d/80-network-retries \
    && apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates python3 perl pkg-config libvulkan-dev \
    libx11-dev libsm-dev libice-dev xz-utils \
    && rm -rf /var/lib/apt/lists/*
COPY 10-env/fetch-qt51510.py 10-env/build-qt51510-host.sh /usr/local/lib/
RUN python3 /usr/local/lib/fetch-qt51510.py /sources \
    && sh /usr/local/lib/build-qt51510-host.sh /sources /qt-build

# Daily IDE image: no system rootfs, vendor materials or preloaded ARM SDK.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
RUN printf 'Acquire::Retries "3";\nAcquire::http::Timeout "30";\nAcquire::https::Timeout "30";\n' > /etc/apt/apt.conf.d/80-network-retries

ENV LANG=C.UTF-8 \
    LANGUAGE=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    TZ=Asia/Shanghai \
    ELF2_DEPENDENCY_ROOT=/opt/elf2-deps \
    ELF2_WORK_CACHE=/cache/elf2-work \
    ELF2_CCACHE_DIR=/cache/elf2-ccache \
    CCACHE_DIR=/cache/elf2-app-ccache \
    ELF2_HOST_QT_PREFIX=/opt/qt-5.15.10 \
    PATH=/opt/qt-5.15.10/bin:$PATH \
    CMAKE_PREFIX_PATH=/opt/qt-5.15.10 \
    QML2_IMPORT_PATH=/opt/qt-5.15.10/qml \
    QT_PLUGIN_PATH=/opt/qt-5.15.10/plugins \
    QT_QPA_PLATFORM=offscreen \
    QT_QUICK_BACKEND=software \
    QT_QPA_FONTDIR=/usr/share/fonts/opentype/noto

COPY --from=qt-host-build /opt/qt-5.15.10 /opt/qt-5.15.10

# Daily development tools. SDK payloads are loaded on demand by elf2-sdk.
RUN apt-get update && apt-get install -y --no-install-recommends \
    bash build-essential ca-certificates ccache cmake ninja-build curl git jq pkg-config \
    python3 rsync unzip xz-utils file e2fsprogs \
    libvulkan-dev mesa-vulkan-drivers vulkan-tools glslang-tools \
    xvfb xauth fonts-dejavu-core fonts-noto-cjk \
    && ln -s /usr/bin/python3 /usr/local/bin/python \
    && rm -rf /var/lib/apt/lists/*

# (4) Python 3 参考实现依赖，直接以 04-algorithms/phase-motion/requirements.txt 为准。
COPY 04-algorithms/phase-motion/requirements.txt /tmp/phase-motion-requirements.txt
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends python3-pip python3-venv python3-dev libglib2.0-0; \
    python3 -m pip install --no-cache-dir --upgrade pip; \
    python3 -m pip install --no-cache-dir -r /tmp/phase-motion-requirements.txt; \
    rm -rf /var/lib/apt/lists/* /tmp/phase-motion-requirements.txt /root/.cache/pip

# (5) Node.js：code-server 扩展安装与 CodeBuddy Code CLI 需要。
COPY 10-env/retry.sh /usr/local/lib/dev-env-retry.sh
RUN set -eux; \
    curl --retry 5 --retry-all-errors -fsSL https://deb.nodesource.com/setup_20.x -o /tmp/nodesource.sh; \
    sh /usr/local/lib/dev-env-retry.sh bash /tmp/nodesource.sh; \
    rm /tmp/nodesource.sh; \
    apt-get install -y --no-install-recommends nodejs; \
    rm -rf /var/lib/apt/lists/* /root/.npm

# (6) code-server、CNB 官方示例中的全部插件，以及 CodeBuddy。
# 装好 code-server 后，CNB 才会以单容器模式启动开发环境。
RUN set -eux; \
    curl --retry 5 --retry-all-errors -fsSL https://code-server.dev/install.sh -o /tmp/code-server-install.sh; \
    sh /usr/local/lib/dev-env-retry.sh sh /tmp/code-server-install.sh; \
    rm /tmp/code-server-install.sh; \
    rm -rf /root/.cache/code-server
RUN set -eux; \
    for ext in \
        cnbcool.cnb-welcome \
        redhat.vscode-yaml \
        dbaeumer.vscode-eslint \
        mhutchie.git-graph \
        ms-python.python \
        llvm-vs-code-extensions.vscode-clangd \
        mathematic.vscode-pdf \
        tencent-cloud.coding-copilot \
    ; do sh /usr/local/lib/dev-env-retry.sh code-server --install-extension "$ext"; done
RUN sh /usr/local/lib/dev-env-retry.sh npm install -g @tencent-ai/codebuddy-code \
    && npm cache clean --force

# (7) 远程接入与常用命令行工具。C/C++ 的 IntelliSense 用 clangd（微软 C/C++ 扩展
# 不在 open-vsx 扩展源内，无法在 code-server 中安装）。
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
        openssh-server sudo less vim tmux htop tree ripgrep shellcheck \
        clangd clang-format; \
    rm -rf /var/lib/apt/lists/*

# (8) 环境变量、CMake 工具链与快捷命令。
COPY 10-env/profile.d/elf2.sh /etc/profile.d/elf2.sh
COPY 10-env/cmake/ /opt/elf2/cmake/
COPY 10-env/bin/elf2-sdk /usr/local/bin/elf2-sdk
COPY 10-env/bin/elf2-system /usr/local/bin/elf2-system
COPY 10-env/bin/elf2-cmake /usr/local/bin/elf2-cmake
RUN set -eux; \
    chmod 0755 /usr/local/bin/elf2-sdk /usr/local/bin/elf2-system /usr/local/bin/elf2-cmake; \
    printf '\n[ -f /etc/profile.d/elf2.sh ] && . /etc/profile.d/elf2.sh\n' >> /etc/bash.bashrc

COPY 10-env/smoke-test.sh /usr/local/lib/dev-env-smoke-test.sh
COPY 10-env/qt-smoke/ /usr/local/lib/elf2-qt-smoke/
COPY 10-env/check-qt.sh /usr/local/lib/check-qt.sh
RUN sh /usr/local/lib/dev-env-smoke-test.sh daily

COPY 10-env/ /tmp/dev-env-check/
RUN python3 -m unittest discover -s /tmp/dev-env-check/tests -v \
    && rm -rf /tmp/dev-env-check

WORKDIR /workspace
