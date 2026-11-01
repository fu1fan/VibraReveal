#!/usr/bin/env python3
"""Add verified vendor Qt development files without replacing target runtime libraries."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

EXPECTED_ARCHIVE_SHA256 = '4a5c3e6deba91a6556917efab471dbffe6beb6c5b6cbb01ec1ffdb3f07b88ea8'


def relocate_mkspecs(host, old_host):
    for config in (host / 'mkspecs').rglob('*'):
        if not config.is_file() or config.suffix not in ('.pri', '.conf', '.prf'):
            continue
        text = config.read_text()
        text = text.replace(old_host + '/aarch64-buildroot-linux-gnu/sysroot', '$$[QT_SYSROOT]')
        text = text.replace(old_host, '$$[QT_HOST_PREFIX]')
        config.write_text(text)
    device = host / 'mkspecs/qdevice.pri'
    text = device.read_text().replace('$$[QT_HOST_PREFIX]/bin/aarch64-buildroot-linux-gnu-',
                                    '$$[QT_HOST_PREFIX]/../toolchain/bin/aarch64-none-linux-gnu-')
    text = text.replace('GCC_MACHINE_DUMP = aarch64-buildroot-linux-gnu',
                        'GCC_MACHINE_DUMP = aarch64-none-linux-gnu')
    device.write_text(text)
    with (host / 'mkspecs/qconfig.pri').open('a') as config:
        config.write('\n!host_build: QMAKE_LFLAGS += -L$$[QT_SYSROOT]/usr/lib -L$$[QT_SYSROOT]/lib\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True)
    parser.add_argument('--vendor', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--rknn-header', type=Path)
    args = parser.parse_args()
    digest = hashlib.sha256()
    with args.archive.open('rb') as archive:
        for chunk in iter(lambda: archive.read(1024 * 1024), b''):
            digest.update(chunk)
    if digest.hexdigest() != EXPECTED_ARCHIVE_SHA256:
        raise SystemExit('Vendor archive checksum mismatch')
    if args.output.exists():
        raise SystemExit('Output exists; use a fresh directory')
    spec = importlib.util.spec_from_file_location('qt_identity', Path(__file__).with_name('qt-runtime-identity.py'))
    identity = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(identity)
    vendor_sysroot = args.vendor / 'aarch64-buildroot-linux-gnu/sysroot'
    runtime = identity.inventory(args.base / 'sysroot/usr/lib')
    development = identity.inventory(vendor_sysroot / 'usr/lib')
    for name in identity.MODULES:
        if runtime[name]['runtime_sha256'] != development[name]['runtime_sha256']:
            raise SystemExit(f'Vendor Qt {name} does not match target runtime')
    old_host = (args.vendor / 'share/buildroot/sdk-location').read_text().strip()
    shutil.copytree(args.base, args.output, symlinks=True)
    (args.output / '.complete').unlink(missing_ok=True)
    target = args.output / 'sysroot'
    shutil.copytree(vendor_sysroot / 'usr/include/qt5', target / 'usr/include/qt5', symlinks=True, dirs_exist_ok=True)
    for include in ('EGL', 'GLES', 'GLES2', 'GLES3', 'KHR'):
        source = vendor_sysroot / 'usr/include' / include
        if source.is_dir():
            shutil.copytree(source, target / 'usr/include' / include, symlinks=True, dirs_exist_ok=True)
    shutil.copytree(vendor_sysroot / 'usr/qml', target / 'usr/qml', symlinks=True, dirs_exist_ok=True)
    # qmake's board EGL configuration links this development-only archive.
    shutil.copy2(vendor_sysroot / 'usr/lib/libmali-hook-injector.a',
                 target / 'usr/lib/libmali-hook-injector.a')
    cmake_root = target / 'usr/lib/cmake'
    for package in (vendor_sysroot / 'usr/lib/cmake').glob('Qt5*'):
        shutil.copytree(package, cmake_root / package.name, symlinks=True, dirs_exist_ok=True)
    for config in cmake_root.glob('Qt5*/*.cmake'):
        text = config.read_text()
        text = text.replace(old_host + '/aarch64-buildroot-linux-gnu/sysroot', '${CMAKE_CURRENT_LIST_DIR}/../../../..')
        text = text.replace(old_host, '${CMAKE_CURRENT_LIST_DIR}/../../../../../qt-host')
        text = text.replace('${_qt5Core_install_prefix}/../../..//mkspecs/', '${_qt5Core_install_prefix}/../../qt-host/mkspecs/')
        config.write_text(text)
    host_bin = args.output / 'qt-host/bin'
    host_bin.mkdir(parents=True)
    shutil.copytree(args.vendor / 'mkspecs', args.output / 'qt-host/mkspecs', symlinks=True)
    relocate_mkspecs(args.output / 'qt-host', old_host)
    for tool in ('moc', 'rcc', 'qmake', 'qmlcachegen'):
        shutil.copy2(args.vendor / 'bin' / tool, host_bin / tool)
    (host_bin / 'qt.conf').write_text('[Paths]\nPrefix=/usr\nHostPrefix=..\nSysroot=../../sysroot\nHeaders=/usr/include/qt5\nPlugins=/usr/lib/qt/plugins\n')
    for tool in ('moc', 'rcc'):
        result = subprocess.run([str(host_bin / tool), '-v'], capture_output=True, text=True, check=True)
        if '5.15.10' not in result.stdout + result.stderr:
            raise SystemExit('Wrong host Qt tool version')
    if args.rknn_header:
        shutil.copy2(args.rknn_header, target / 'usr/include/rknn_api.h')
    # All original target libraries must remain byte-for-byte unchanged.
    if identity.inventory(target / 'usr/lib') != runtime:
        raise SystemExit('Target Qt runtime was changed during SDK augmentation')
    manifest_path = args.output / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['qt'] = {'version': '5.15.10', 'vendor_archive_sha256': EXPECTED_ARCHIVE_SHA256,
                      'runtime': runtime, 'vendor': development, 'host_tools': 'qt-host/bin',
                      'qmake_relocatable': True}
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print('QT_SDK_AUGMENTED_RUNTIME_UNCHANGED')


if __name__ == '__main__':
    main()
