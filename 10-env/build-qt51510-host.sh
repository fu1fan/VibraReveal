#!/bin/sh
# Linux/amd64 HEADLESS host Qt from official Qt 5.15.10 release archives.
# This is not the AArch64 SDK and does not replace the board's Qt.
set -eu
sources=${1:-/sources}
work=${2:-/qt-build}
prefix=/opt/qt-5.15.10
mkdir -p "$work"
cd "$sources"
# SHA256 values from https://download.qt.io/archive/qt/5.15/5.15.10/submodules/
printf '%s\n' \
 'c0d06cb18d20f10bf7ad53552099e097ec39362d30a5d6f104724f55fa1c8fb9  qtbase-everywhere-opensource-src-5.15.10.tar.xz' \
 '239b4c3c4cc49b174391445ad4b8320c11c8655300977955febd5dde69d33df4  qtdeclarative-everywhere-opensource-src-5.15.10.tar.xz' \
 '97954c92fee9031ec31341812ccf53afb3c132e2b271bcef1174367fa002fc8e  qtquickcontrols2-everywhere-opensource-src-5.15.10.tar.xz' | sha256sum -c -
for module in qtbase qtdeclarative qtquickcontrols2; do
    spec="$module-everywhere-src-5.15.10"
    if [ ! -d "$work/$spec" ]; then
        tar -xf "$sources/$module-everywhere-opensource-src-5.15.10.tar.xz" -C "$work"
    fi
    cd "$work/$spec"
    grep -q '^MODULE_VERSION = 5.15.10$' .qmake.conf
    case "$spec" in
        qtbase-*)
            if [ ! -f Makefile ]; then
                ./configure -prefix "$prefix" -opensource -confirm-license \
                    -release -nomake examples -nomake tests -no-opengl -no-xcb \
                    -no-dbus -no-icu -no-glib -qt-freetype -no-fontconfig
            fi
            ;;
        *) "$prefix/bin/qmake" "QT_BUILD_PARTS=libs tools" ;;
    esac
    make -j"${QT_BUILD_JOBS:-4}"
    make install
done
[ "$("$prefix/bin/qmake" -query QT_VERSION)" = 5.15.10 ]
"$prefix/bin/moc" -v
"$prefix/bin/rcc" -v
echo QT_5_15_10_HEADLESS_HOST_BUILT
