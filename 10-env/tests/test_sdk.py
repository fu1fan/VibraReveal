import importlib.machinery
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

loader = importlib.machinery.SourceFileLoader('sdk_client', str(Path(__file__).resolve().parents[1] / 'bin/elf2-sdk'))
spec = importlib.util.spec_from_loader(loader.name, loader)
sdk = importlib.util.module_from_spec(spec)
loader.exec_module(sdk)

class Client(unittest.TestCase):
    def fixture(self, root):
        lock = root / 'sdk.lock.json'
        spec = {'image': 'example.test/app-sdk@sha256:' + 'a'*64, 'system_commit': 'b'*40}
        lock.write_text(json.dumps(spec))
        return lock, spec

    def populate(self, dest, commit):
        (dest / 'toolchain/bin').mkdir(parents=True)
        (dest / 'toolchain/bin/aarch64-none-linux-gnu-gcc').write_text('compiler')
        (dest / 'sysroot/usr/include').mkdir(parents=True)
        (dest / 'manifest.json').write_text(json.dumps({'schema': 1, 'system_commit': commit}))

    def test_cache_works_without_docker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock, spec = self.fixture(root)
            dest = root / 'cache' / ('a'*64)
            self.populate(dest, spec['system_commit'])
            (dest / '.complete').write_text(spec['image']+'\n')
            with patch.object(sdk, 'run', side_effect=AssertionError('Docker accessed on cache hit')):
                self.assertEqual(sdk.load(lock, root / 'cache'), dest)
            env = sdk.environment(dest)
            self.assertEqual(env['PKG_CONFIG_PATH'], '')
            self.assertTrue(env['ELF2_SYSROOT'].endswith('/sysroot'))

    def test_failed_copy_never_publishes_cache_and_removes_container(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock, spec = self.fixture(root)
            calls = []
            def run(*args, **kwargs):
                calls.append(args)
                if args[:2] == ('docker', 'cp'):
                    raise RuntimeError('disk full')
            with patch.object(sdk, 'run', side_effect=run), patch.object(sdk.subprocess, 'check_output', return_value='container\n'):
                with self.assertRaises(RuntimeError): sdk.load(lock, root / 'cache')
            self.assertFalse((root / 'cache' / ('a'*64)).exists())
            self.assertIn(('docker', 'rm', 'container'), calls)

    def test_wrong_system_and_mutable_tags_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock, spec = self.fixture(root)
            self.populate(root / 'sdk', 'wrong')
            with self.assertRaises(RuntimeError): sdk.validate(root / 'sdk', spec)
            spec['image'] = 'example.test/app-sdk:latest'
            lock.write_text(json.dumps(spec))
            with self.assertRaises(ValueError): sdk.load(lock, root / 'cache')
