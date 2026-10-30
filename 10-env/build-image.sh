#!/usr/bin/env bash
# 组装最小构建上下文并构建开发环境镜像。
#
# 仓库根目录包含 2.4 GB 的 02-platform/RK3588-OV9281 子模块、原理图与 CAD 资料，
# 直接作为上下文会让 docker build 变慢甚至超时。这里只复制 Dockerfile 需要的文件。
#
# 用法：
#   IMAGE=docker.cnb.cool/fu1fan/vibrareveal/dev-env:latest bash 10-env/build-image.sh
#   IMAGE=... bash 10-env/build-image.sh docker.cnb.cool/fu1fan/vibrareveal/dev-env:ci-abc1234
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
SDK=$ROOT/02-platform/RK3588-OV9281

MODE=${MODE:-daily}
case "$MODE" in daily) DOCKERFILE=Dockerfile ;; system) DOCKERFILE=Dockerfile.system ;; *) exit 2 ;; esac
if [ "$MODE" = system ] && [ ! -f "$SDK/.ci/fetch_deps.py" ]; then
    echo 'Initialize the pinned RK3588 submodule before building the system environment' >&2
    exit 1
fi

CONTEXT=$(mktemp -d)
trap 'rm -rf "$CONTEXT"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

mkdir -p "$CONTEXT/10-env" \
         "$CONTEXT/02-platform/RK3588-OV9281/.ci" \
         "$CONTEXT/02-platform/RK3588-OV9281/docs/migration" \
         "$CONTEXT/04-algorithms/phase-motion"
cp -a "$HERE/." "$CONTEXT/10-env/"
if [ "$MODE" = system ]; then
cp -a "$SDK/.ci/fetch_deps.py" "$CONTEXT/02-platform/RK3588-OV9281/.ci/"
cp -a "$SDK/docs/migration/dependencies.json" "$CONTEXT/02-platform/RK3588-OV9281/docs/migration/"
fi
cp -a "$ROOT/04-algorithms/phase-motion/requirements.txt" "$CONTEXT/04-algorithms/phase-motion/"

IMAGE=${IMAGE:-vibrareveal-dev-env:local}
tags=(-t "$IMAGE")
for extra in "$@"; do
    tags+=(-t "$extra")
done

echo "构建上下文：$CONTEXT"
echo "镜像标签：${tags[*]}"
docker build --platform linux/amd64 "${tags[@]}" -f "$CONTEXT/10-env/$DOCKERFILE" "$CONTEXT"
