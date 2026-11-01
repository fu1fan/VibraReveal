#!/usr/bin/env python3
"""Compare ELF 2 Qt loadable sections, allowing non-runtime stripping differences."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

MODULES = ('Core', 'Gui', 'Qml', 'Quick', 'QuickControls2', 'QuickTemplates2')


def identity(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or len(data) < 64:
        raise ValueError(f'Not a 64-bit little-endian ELF: {path}')
    header = struct.unpack_from('<16sHHIQQQIHHHHHH', data)
    if header[2] != 183:
        raise ValueError(f'Not AArch64: {path}')
    offset, size, count, names_index = header[6], header[11], header[12], header[13]
    if size != 64 or not count or names_index >= count or offset + count * size > len(data):
        raise ValueError(f'Invalid section table: {path}')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, offset + i * size) for i in range(count)]
    names = sections[names_index]
    strings = data[names[4]:names[4] + names[5]]
    digest = hashlib.sha256()
    for section in sections:
        name, kind, flags, address, start, length, link, info, alignment, entry_size = section
        if not flags & 2:  # SHF_ALLOC: retain all sections participating in runtime image
            continue
        if name >= len(strings) or (kind != 8 and start + length > len(data)):
            raise ValueError(f'Invalid allocated section: {path}')
        label = strings[name:].split(b'\0', 1)[0]
        digest.update(label + b'\0')
        digest.update(struct.pack('<IQQQQQ', kind, flags, address, length, alignment, entry_size))
        if kind != 8:  # SHT_NOBITS still contributes its size and layout above
            digest.update(data[start:start + length])
    return {'file_sha256': hashlib.sha256(data).hexdigest(), 'runtime_sha256': digest.hexdigest()}


def inventory(directory):
    return {name: identity(directory / f'libQt5{name}.so.5.15.10') for name in MODULES}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path, help='Directory containing actual board Qt libraries')
    parser.add_argument('--compare', type=Path, help='Vendor SDK library directory')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = {'qt_version': '5.15.10', 'runtime': inventory(args.runtime)}
    if args.compare:
        report['sdk'] = inventory(args.compare)
        report['matching_runtime_sections'] = all(
            report['runtime'][name]['runtime_sha256'] == report['sdk'][name]['runtime_sha256']
            for name in MODULES)
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')
    if args.compare and not report['matching_runtime_sections']:
        raise SystemExit('Qt runtime section mismatch; do not publish as board-matched SDK')


if __name__ == '__main__':
    main()
