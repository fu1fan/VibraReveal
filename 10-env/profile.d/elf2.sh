# ELF2 / RK3588 交叉编译与增量构建环境（10-env 镜像内置）。
# 登录 shell 与 /etc/bash.bashrc 都会加载本文件。

# ELF2 固定依赖（工具链、rootfs、厂商材料）的展开目录，与 .ci/build.py 使用的变量同名。
export ELF2_DEPENDENCY_ROOT="${ELF2_DEPENDENCY_ROOT:-/opt/elf2-deps}"
export ELF2_ROOT="$ELF2_DEPENDENCY_ROOT"
export ELF2_SDK_DIR="${ELF2_SDK_DIR:-/workspace/02-platform/RK3588-OV9281}"
export ELF2_WORK_CACHE="${ELF2_WORK_CACHE:-/cache/elf2-work}"
export ELF2_CCACHE_DIR="${ELF2_CCACHE_DIR:-/cache/elf2-ccache}"
export CCACHE_DIR="${CCACHE_DIR:-/cache/elf2-app-ccache}"

# 交叉工具链的版本目录名固定为厂商命名，这里按预先固化的路径探测，避免硬编码版本号。
if [ -z "${ELF2_CROSS_COMPILE:-}" ]; then
    for _elf2_gcc in "$ELF2_DEPENDENCY_ROOT"/prebuilts/gcc/linux-x86/aarch64/*/bin/aarch64-none-linux-gnu-gcc; do
        [ -x "$_elf2_gcc" ] || continue
        ELF2_CROSS_COMPILE="${_elf2_gcc%gcc}"
        _elf2_bin=$(dirname "$_elf2_gcc")
        case ":$PATH:" in
            *":$_elf2_bin:"*) ;;
            *) PATH="$_elf2_bin:$PATH" ;;
        esac
        break
    done
    unset _elf2_gcc _elf2_bin
fi
export ELF2_CROSS_COMPILE
export PATH

if [ -n "${ELF2_CROSS_COMPILE:-}" ] && [ -z "${ELF2_SYSROOT:-}" ]; then
    _elf2_sysroot=$("${ELF2_CROSS_COMPILE}gcc" -print-sysroot 2>/dev/null || true)
    if [ -n "$_elf2_sysroot" ] && [ -d "$_elf2_sysroot" ]; then
        ELF2_SYSROOT=$_elf2_sysroot
    fi
    unset _elf2_sysroot
fi
export ELF2_SYSROOT="${ELF2_SYSROOT:-}"
