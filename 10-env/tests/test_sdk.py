import importlib.machinery
import importlib.util
import json
import os
import subprocess
import sys
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

    def test_two_processes_install_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lock, spec = self.fixture(root)
            self.populate(root / 'payload', spec['system_commit'])
            fake = root / 'docker'
            fake.write_text('#!' + sys.executable + '\n' + """import os, pathlib, shutil, sys, time
root = pathlib.Path(os.environ['FIXTURE'])
args = sys.argv[1:]
if args[0] == 'pull':
    with (root / 'pulls').open('a') as f: f.write('pull\\n')
    time.sleep(.3)
elif args[0] == 'create': print('container')
elif args[0] == 'cp': shutil.copytree(root / 'payload', args[-1])
""")
            fake.chmod(0o755)
            env = dict(os.environ, FIXTURE=str(root), PATH=str(root)+':'+os.environ['PATH'])
            cmd = [sys.executable, loader.path, '--lock', str(lock), '--cache', str(root / 'cache'), 'use']
            procs = [subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
            for proc in procs:
                out, err = proc.communicate(timeout=10)
                self.assertEqual(proc.returncode, 0, err.decode())
            self.assertEqual((root / 'pulls').read_text().splitlines(), ['pull'])
