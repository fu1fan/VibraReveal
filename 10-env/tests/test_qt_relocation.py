import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('qt_augment', Path(__file__).parents[1] / 'augment-qt-sdk.py')
augment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(augment)


class QmakeRelocation(unittest.TestCase):
    def test_vendor_paths_and_runtime_library_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            host = Path(tmp)
            specs = host / 'mkspecs'
            specs.mkdir()
            old = '/vendor/build/host'
            (specs / 'qdevice.pri').write_text(
                f'CROSS_COMPILE = {old}/bin/aarch64-buildroot-linux-gnu-\n'
                'GCC_MACHINE_DUMP = aarch64-buildroot-linux-gnu\n')
            (specs / 'qconfig.pri').write_text(
                f'PKG_CONFIG_SYSROOT_DIR = {old}/aarch64-buildroot-linux-gnu/sysroot\n')
            augment.relocate_mkspecs(host, old)
            device = (specs / 'qdevice.pri').read_text()
            config = (specs / 'qconfig.pri').read_text()
            self.assertNotIn(old, device + config)
            self.assertIn('$$[QT_HOST_PREFIX]/../toolchain/bin/aarch64-none-linux-gnu-', device)
            self.assertIn('PKG_CONFIG_SYSROOT_DIR = $$[QT_SYSROOT]', config)
            self.assertIn('!host_build: QMAKE_LFLAGS += -L$$[QT_SYSROOT]/usr/lib', config)
