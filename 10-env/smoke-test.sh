#!/bin/sh
set -eu
# shellcheck source=/dev/null
. /etc/profile.d/elf2.sh
if [ "${1:-system}" = system ]; then
[ -s "$ELF2_DEPENDENCY_ROOT/.manifest-sha256" ]
[ "$("${ELF2_CROSS_COMPILE}gcc" -dumpmachine)" = aarch64-none-linux-gnu ]
fi
python3 -c 'import numpy, cv2, matplotlib'
python -c 'import sys; assert sys.version_info.major == 3'
if [ "${1:-system}" = system ]; then
    USER=elf2-ci python -c 'import sys; assert sys.version_info.major == 2'
fi
command -v codebuddy
command -v clangd
command -v flock
extensions=$(code-server --list-extensions)
for ext in cnbcool.cnb-welcome redhat.vscode-yaml dbaeumer.vscode-eslint mhutchie.git-graph ms-python.python llvm-vs-code-extensions.vscode-clangd mathematic.vscode-pdf tencent-cloud.coding-copilot; do
    printf '%s\n' "$extensions" | grep -Fxi "$ext"
done
if [ "${1:-system}" = system ]; then
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
printf 'int main(void) { return 0; }\n' > "$scratch/main.c"
"${ELF2_CROSS_COMPILE}gcc" "$scratch/main.c" -o "$scratch/test"
file "$scratch/test" | grep 'ARM aarch64'
fi
echo 'Development image smoke test passed'
