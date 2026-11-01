import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('qt_identity', Path(__file__).parents[1] / 'qt-runtime-identity.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def elf(code=b'CODE', debug=b'DEBUG', machine=183):
    names = b'\0.shstrtab\0.text\0.debug\0'
    payload = names + code + debug
    table = 64 + len(payload)
    ident = b'\x7fELF\x02\x01\x01' + bytes(9)
    header = struct.pack('<16sHHIQQQIHHHHHH', ident, 3, machine, 1, 0, 0, table, 0, 64, 0, 0, 64, 4, 1)
    sections = [(0,) * 10, (1, 3, 0, 0, 64, len(names), 0, 0, 1, 0),
                (11, 1, 6, 4096, 64 + len(names), len(code), 0, 0, 4, 0),
                (17, 1, 0, 0, 64 + len(names) + len(code), len(debug), 0, 0, 1, 0)]
    return header + payload + b''.join(struct.pack('<IIQQQQIIQQ', *s) for s in sections)


class RuntimeIdentity(unittest.TestCase):
    def check(self, data):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'library.so'
            path.write_bytes(data)
            return module.identity(path)

    def test_non_runtime_stripping_does_not_change_identity(self):
        first, second = self.check(elf()), self.check(elf(debug=b''))
        self.assertNotEqual(first['file_sha256'], second['file_sha256'])
        self.assertEqual(first['runtime_sha256'], second['runtime_sha256'])

    def test_changed_runtime_code_is_rejected(self):
        self.assertNotEqual(self.check(elf())['runtime_sha256'], self.check(elf(code=b'EVIL'))['runtime_sha256'])

    def test_wrong_architecture_and_truncated_elf_are_rejected(self):
        for data in (elf(machine=62), elf()[:-4], b'not ELF'):
            with self.assertRaises(ValueError):
                self.check(data)
