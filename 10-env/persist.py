#!/usr/bin/env python3
"""Link IDE state to CNB roaming storage without deleting the original data."""
import fcntl
import os
from pathlib import Path
import shutil
import sys
import uuid

SKIP = set('logs traces debug diagnostics blobs file-history sessions shell-snapshots plugins'.split())


def exists(path):
    return os.path.lexists(path)


def copy_missing(source, dest):
    # Never follow a destination symlink when merging restored data.
    if exists(dest):
        if source.is_dir() and not source.is_symlink() and dest.is_dir() and not dest.is_symlink():
            for child in source.iterdir():
                copy_missing(child, dest / child.name)
        return
    if source.is_symlink():
        value = os.readlink(source)
        dest.symlink_to(os.path.abspath(source.parent / value) if not os.path.isabs(value) else value)
    elif source.is_dir():
        dest.mkdir()
        for child in source.iterdir():
            copy_missing(child, dest / child.name)
    elif source.is_file():
        # A failed copy must not leave a partial file that a retry mistakes for saved state.
        temporary = dest.with_name(dest.name + '.copy-' + uuid.uuid4().hex)
        try:
            shutil.copy2(source, temporary)
            os.replace(temporary, dest)
        finally:
            if exists(temporary):
                temporary.unlink()
    else:
        raise RuntimeError(f'Unsupported state entry: {source}')


def link(target, dest, directory=False):
    target.parent.mkdir(parents=True, exist_ok=True)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink():
        # Preserve existing external links (e.g. skills -> ../.agents/skills).
        return
    if exists(dest) and (dest.is_symlink() or dest.is_dir() != directory):
        raise RuntimeError(f'Unexpected roaming entry type: {dest}')
    if exists(target):
        if target.is_dir() != directory:
            raise RuntimeError(f'Unexpected source entry type: {target}')
        copy_missing(target, dest)
    elif directory:
        dest.mkdir(exist_ok=True)
    backup = None
    if exists(target):
        # Retain conflicting values and a recoverable original even if linking fails.
        backup = target.with_name(target.name + '.pre-roam-' + uuid.uuid4().hex)
        target.rename(backup)
    try:
        target.symlink_to(dest, target_is_directory=directory)
    except BaseException:
        if backup is not None:
            backup.rename(target)
        raise
    if backup is not None:
        print(f'Original retained: {backup}')


def restore(home, repo):
    parts = Path(repo).parts
    if not parts or Path(repo).is_absolute() or '..' in parts:
        raise ValueError('Invalid repository slug')
    roam = home / '.cnb' / repo
    roam.mkdir(parents=True, exist_ok=True)
    with (roam / '.persist.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        user = home / '.local/share/code-server/User'
        for target, relative in [
            (user / 'globalStorage/tencent-cloud.coding-copilot', 'code-server/globalStorage/tencent-cloud.coding-copilot'),
            (user / 'History', 'code-server/History'),
            (home / '.agents', 'agents'),
            (home / '.local/share/CodeBuddyExtension/Data', 'CodeBuddyExtension/Data'),
        ]:
            link(target, roam / relative, True)
        for target, relative in [
            (user / 'Machine/settings.json', 'code-server/Machine/settings.json'),
            (home / '.vscode-server/data/Machine/settings.json', 'vscode-server/Machine/settings.json'),
        ]:
            link(target, roam / relative)
        cli = home / '.codebuddy'
        saved = roam / 'codebuddy'
        names = set('settings.json settings.local.json CODEBUDDY.md mcp.json history.jsonl expert-history.json'.split())
        for parent in (cli, saved):
            if parent.is_dir():
                names.update(p.name for p in parent.iterdir())
        for name in sorted(names - SKIP):
            if '.pre-roam-' in name or '.copy-' in name:
                continue
            target, dest = cli / name, saved / name
            directory = target.is_dir() if exists(target) else dest.is_dir()
            link(target, dest, directory)
    return roam


if __name__ == '__main__':
    home = Path.home()
    repo = os.environ.get('CNB_REPO_SLUG', 'VibraReveal')
    if sys.argv[1:] == ['--report']:
        roam = home / '.cnb' / repo
    elif not sys.argv[1:]:
        roam = restore(home, repo)
    else:
        sys.exit('Usage: persist.sh [--report]')
    print(f'Roaming directory: {roam}')
    if roam.is_dir():
        import subprocess
        subprocess.run(['du', '-sh', str(roam)], check=True)
