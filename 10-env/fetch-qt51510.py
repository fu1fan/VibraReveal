#!/usr/bin/env python3
"""Fetch the official, checksum-pinned host Qt sources (not an ARM SDK)."""
import hashlib
import os
from pathlib import Path
import sys
import time
import urllib.request

BASE = 'https://download.qt.io/archive/qt/5.15/5.15.10/submodules/'
HASHES = {
    'qtbase': 'c0d06cb18d20f10bf7ad53552099e097ec39362d30a5d6f104724f55fa1c8fb9',
    'qtdeclarative': '239b4c3c4cc49b174391445ad4b8320c11c8655300977955febd5dde69d33df4',
    'qtquickcontrols2': '97954c92fee9031ec31341812ccf53afb3c132e2b271bcef1174367fa002fc8e',
}


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(source.read()).hexdigest()


def fetch(destination):
    destination.mkdir(parents=True, exist_ok=True)
    for module, expected in HASHES.items():
        path = destination / f'{module}-everywhere-opensource-src-5.15.10.tar.xz'
        if path.exists() and digest(path) == expected:
            continue
        partial = path.with_name(path.name + f'.partial-{os.getpid()}')
        for attempt in range(3):
            try:
                with urllib.request.urlopen(BASE + path.name, timeout=60) as source, partial.open('wb') as output:
                    while block := source.read(1024 * 1024):
                        output.write(block)
                if digest(partial) != expected:
                    raise RuntimeError(f'Checksum mismatch: {module}')
                partial.replace(path)
                break
            except Exception:
                partial.unlink(missing_ok=True)
                if attempt == 2:
                    raise
                time.sleep(attempt + 1)


if __name__ == '__main__':
    fetch(Path(sys.argv[1]))
