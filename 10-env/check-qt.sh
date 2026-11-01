#!/bin/sh
# Host QML smoke or explicit target-SDK compile gate. Never runs target code.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir="$here/qt-smoke"
[ -d "$source_dir" ] || source_dir="$here/elf2-qt-smoke"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
case "${1:-host}" in
    host)
        cmake -S "$source_dir" -B "$scratch/build" -G Ninja
        cmake --build "$scratch/build"
        mkdir "$scratch/runtime"
        chmod 700 "$scratch/runtime"
        XDG_RUNTIME_DIR="$scratch/runtime" QT_QPA_PLATFORM=offscreen \
            QT_QUICK_BACKEND=software "$scratch/build/qt-smoke"
        echo QT_HOST_QML_VERIFIED
        ;;
    sdk)
        toolchain="$here/cmake/elf2-aarch64-toolchain.cmake"
        [ -f "$toolchain" ] || toolchain=/opt/elf2/cmake/elf2-aarch64-toolchain.cmake
        elf2-cmake -S "$source_dir" -B "$scratch/build" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="$toolchain"
        elf2-sdk run cmake --build "$scratch/build"
        file "$scratch/build/qt-smoke" | grep 'ARM aarch64'
        echo QT_SDK_LINK_VERIFIED_NOT_BOARD_RUNTIME
        ;;
    *) echo 'Usage: check-qt.sh host|sdk' >&2; exit 2 ;;
esac
