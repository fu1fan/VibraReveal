#!/bin/sh
# Target build only: requires the board-matched Qt 5.15.10 development SDK.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-/tmp/vibrareveal-touch-build}
shift $(($# > 0 ? 1 : 0))
elf2-cmake -S "$root/03-apps/rk3588" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/10-env/cmake/elf2-aarch64-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DVR_BUILD_TOUCH_UI=ON -DVR_BUILD_RKNN_SELECTION=ON -DBUILD_TESTING=OFF "$@"
elf2-sdk run cmake --build "$build" --parallel 4
file "$build/vr-touch" | grep 'ARM aarch64'
elf2-sdk run cmake --install "$build" --prefix "$build/package/usr/local"
python3 - "$root" "$build" <<'PY'
import hashlib, json, pathlib, subprocess, sys
root, build = map(pathlib.Path, sys.argv[1:])
metadata = {
    'application_commit': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
    'dirty': bool(subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain', '--', '03-apps/rk3588', '03-apps/raw-codec'], text=True).strip()),
    'required_qt': '5.15.10',
    'sdk': json.loads((root / '10-env/sdk.lock.json').read_text()),
    'sha256': hashlib.sha256((build / 'vr-touch').read_bytes()).hexdigest(),
    'shader_sha256': hashlib.sha256((build / 'motion.comp.spv').read_bytes()).hexdigest(),
    'board_runtime_verified': False,
}
(build / 'touch-build-info.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata, indent=2))
PY
