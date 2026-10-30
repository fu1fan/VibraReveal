import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest
from unittest.mock import patch

ENV = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('persist', ENV / 'persist.py')
persist = importlib.util.module_from_spec(spec)
spec.loader.exec_module(persist)


class Persistence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def test_copy_failure_keeps_original_and_retry_copies_complete_file(self):
        src, dst = self.root / 'source', self.root / 'saved'
        src.mkdir()
        (src / 'data').write_text('complete')
        def fail(source, target):
            Path(target).write_text('part')
            raise OSError('disk full')
        with patch.object(persist.shutil, 'copy2', side_effect=fail):
            with self.assertRaises(OSError):
                persist.link(src, dst, True)
        self.assertFalse(src.is_symlink())
        self.assertEqual((src / 'data').read_text(), 'complete')
        self.assertFalse((dst / 'data').exists())
        persist.link(src, dst, True)
        self.assertEqual((dst / 'data').read_text(), 'complete')

    def test_conflict_preserves_both_versions_and_external_links(self):
        src, dst = self.root / 'source', self.root / 'saved'
        src.write_text('local')
        dst.write_text('roamed')
        persist.link(src, dst)
        self.assertEqual(src.read_text(), 'roamed')
        self.assertEqual(next(self.root.glob('source.pre-roam-*')).read_text(), 'local')
        external = self.root / 'skills'
        external.symlink_to('source')
        persist.link(external, self.root / 'other')
        self.assertEqual(os.readlink(external), 'source')

    def test_restore_dynamic_and_hidden_entries_in_new_environment(self):
        saved = self.root / '.cnb/test/codebuddy/projects'
        saved.mkdir(parents=True)
        (saved / 'chat').write_text('history')
        (saved.parent / '.preferences').write_text('prefs')
        persist.restore(self.root, 'test')
        persist.restore(self.root, 'test')
        self.assertEqual((self.root / '.codebuddy/projects/chat').read_text(), 'history')
        self.assertEqual((self.root / '.codebuddy/.preferences').read_text(), 'prefs')

    def test_relative_nested_link_survives_move(self):
        src = self.root / 'source'
        src.mkdir()
        (src / 'data').write_text('value')
        (src / 'link\nname').symlink_to('data')
        persist.link(src, self.root / 'saved', True)
        self.assertEqual((src / 'link\nname').read_text(), 'value')

    def test_link_failure_rolls_back(self):
        src = self.root / 'source'
        src.write_text('value')
        with patch.object(Path, 'symlink_to', side_effect=OSError('denied')):
            with self.assertRaises(OSError):
                persist.link(src, self.root / 'saved')
        self.assertEqual(src.read_text(), 'value')
        self.assertFalse(src.is_symlink())


@unittest.skipUnless(shutil.which('flock'), 'Linux flock required')
class BuildLock(unittest.TestCase):
    def test_concurrent_build_rejected_until_finish(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / '.ci').mkdir()
            (root / '.ci/build.py').write_text('''import pathlib, sys, time
p = pathlib.Path('events')
with p.open('a') as f: f.write('finish\\n' if '--finish' in sys.argv else 'build\\n')
if '--finish' not in sys.argv: time.sleep(2)
''')
            (root / '.gitignore').write_text('events\n')
            subprocess.run(['git', 'init', '-q', str(root)], check=True)
            subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'fixture'], check=True)
            env = dict(os.environ, ELF2_SDK_DIR=str(root), ELF2_WORK_CACHE=str(root / '.git/cache'))
            first = subprocess.Popen(['sh', str(ENV / 'bin/elf2-build')], env=env)
            try:
                for _ in range(100):
                    if (root / 'events').exists(): break
                    time.sleep(.02)
                self.assertTrue((root / 'events').exists())
                second = subprocess.run(['sh', str(ENV / 'bin/elf2-build')], env=env)
                self.assertNotEqual(second.returncode, 0)
                # A different cache still must not overwrite SDK dist/state.
                second = subprocess.run(['sh', str(ENV / 'bin/elf2-build')], env=dict(env, ELF2_WORK_CACHE=str(root / '.git/other')))
                self.assertNotEqual(second.returncode, 0)
                self.assertEqual(first.wait(timeout=10), 0)
                self.assertEqual((root / 'events').read_text(), 'build\nfinish\n')
                self.assertNotEqual(subprocess.run(['sh', str(ENV / 'bin/elf2-build'), '--finish'], env=env).returncode, 0)
            finally:
                if first.poll() is None: first.kill(); first.wait()


@unittest.skipUnless(shutil.which('debugfs') and shutil.which('flock'), 'Linux ext4 tools required')
class Sysroot(unittest.TestCase):
    def test_real_ext4_and_changed_baseline(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            tree = root / 'tree'
            (tree / 'usr').mkdir(parents=True)
            (tree / 'lib').mkdir()
            (tree / 'usr/payload').write_text('payload')
            img = root / 'rootfs.ext4'
            subprocess.run(['truncate', '-s', '16M', str(img)], check=True)
            subprocess.run(['mkfs.ext4', '-q', '-d', str(tree), str(img)], check=True)
            dest = root / 'sysroot'
            env = dict(os.environ, ELF2_ROOTFS_IMAGE=str(img))
            cmd = ['sh', str(ENV / 'bin/elf2-sysroot'), str(dest)]
            subprocess.run(cmd, env=env, check=True)
            self.assertEqual((dest / 'usr/payload').read_text(), 'payload')
            subprocess.run(cmd, env=env, check=True)
            img.write_bytes(b'broken image')
            self.assertNotEqual(subprocess.run(cmd, env=env).returncode, 0)
            bad = root / 'bad'
            self.assertNotEqual(subprocess.run(cmd[:-1] + [str(bad)], env=env).returncode, 0)
            self.assertFalse((bad / '.complete').exists())

    def test_debugfs_zero_exit_error_is_not_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fake = root / 'debugfs'
            fake.write_text('#!/bin/sh\necho "rdump: No space left on device" >&2\nexit 0\n')
            fake.chmod(0o755)
            img = root / 'image'
            img.write_text('image')
            dest = root / 'dest'
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'], ELF2_ROOTFS_IMAGE=str(img))
            self.assertNotEqual(subprocess.run(['sh', str(ENV / 'bin/elf2-sysroot'), str(dest)], env=env).returncode, 0)
            self.assertFalse((dest / '.complete').exists())


class Retry(unittest.TestCase):
    def test_transient_failure_and_exhaustion(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'sleep').write_text('#!/bin/sh\nexit 0\n')
            (root / 'sleep').chmod(0o755)
            command = root / 'install'
            command.write_text('#!/bin/sh\nn=$(cat "$COUNT" 2>/dev/null || echo 0)\nn=$((n + 1))\necho "$n" > "$COUNT"\n[ "$n" -ge "$SUCCEED_AT" ]\n')
            command.chmod(0o755)
            count = root / 'count'
            env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'], COUNT=str(count), SUCCEED_AT='3')
            cmd = ['sh', str(ENV / 'retry.sh'), str(command)]
            self.assertEqual(subprocess.run(cmd, env=env).returncode, 0)
            self.assertEqual(count.read_text().strip(), '3')
            count.unlink()
            self.assertNotEqual(subprocess.run(cmd, env=dict(env, SUCCEED_AT='99')).returncode, 0)
            self.assertEqual(count.read_text().strip(), '5')


if __name__ == '__main__':
    unittest.main()
