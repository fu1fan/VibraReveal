#!/bin/sh
# Run in the CNB daily environment. Never build RK3588 binaries on the Mac.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=${1:-/tmp/vibrareveal-image-transport-build}
elf2-cmake -S "$root/03-apps/rk3588" -B "$build" -DCMAKE_BUILD_TYPE=Release
elf2-sdk run cmake --build "$build" --parallel 4
file "$build/vr-stream" | grep 'ARM aarch64'
python3 - "$root" "$build" <<'PY'
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
root, build = map(pathlib.Path, sys.argv[1:])
binary = build / 'vr-stream'
shutil.copytree(root / '03-apps/rk3588/scripts', build / 'scripts', dirs_exist_ok=True)
metadata = {
    'application_commit': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
    'dirty': bool(subprocess.check_output(['git', '-C', str(root), 'status', '--porcelain', '--', '03-apps/rk3588', '10-env/sdk.lock.json'], text=True).strip()),
    'scripts_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((build / 'scripts').iterdir())},
    'sdk': json.loads((root / '10-env/sdk.lock.json').read_text()),
    'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'clock_probe_sha256': hashlib.sha256((build / 'vr-clock-probe').read_bytes()).hexdigest(),
    'bulk_benchmark_sha256': hashlib.sha256((build / 'vr-bulk-source').read_bytes()).hexdigest(),
}
(build / 'build-info.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata, indent=2))
PY
