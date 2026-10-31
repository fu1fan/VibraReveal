#!/usr/bin/env python3
"""Independent GPU edge reference and archive/CLI acceptance. Requires numpy.
Usage: python3 06-tests/rk3588_backend.py BUILD_DIRECTORY
"""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
import numpy as np

build = Path(sys.argv[1]).resolve()
shader = str(build / 'motion.comp.spv')


def run(tool, args, ok=True):
    p = subprocess.run([str(build / tool)] + args, capture_output=True, timeout=120)
    assert (p.returncode == 0) == ok, (tool, p.returncode, p.stderr.decode())
    return p


# Independent separable blur and Sobel reference (zero derivative on reflected borders).
w, h = 64, 48
image = np.tile(np.where(np.arange(w) < 32, 100., 900.) / 1023, (h, 1))

def conv(a, weights, axis):
    r = len(weights) // 2
    pads = [(0, 0), (0, 0)]; pads[axis] = (r, r)
    padded = np.pad(a, pads, mode='reflect')
    return sum(weight * np.take(padded, range(i, i + a.shape[axis]), axis=axis)
               for i, weight in enumerate(weights))

blur = conv(conv(image, np.array([1, 4, 6, 4, 1]) / 16, 1), np.array([1, 4, 6, 4, 1]) / 16, 0)
gx = conv(conv(blur, [-1, 0, 1], 1), [.25, .5, .25], 0)
gy = conv(conv(blur, [-1, 0, 1], 0), [.25, .5, .25], 1)
mag = np.hypot(gx, gy)
raw = (build / 'features-test.bin').read_bytes()
actual = np.frombuffer(raw[:w*h*12], '<f4').reshape(3, h, w)
assert np.max(np.abs(actual[0] - mag)) < 1e-6
assert np.max(np.abs(actual[1][mag > .01] - (gx / np.maximum(mag, 1e-8))[mag > .01])) < 1e-5
peaks, edges, regions = np.frombuffer(raw[w*h*12:], 'u1').reshape(3, h, w)
assert np.any(peaks) and np.any(edges) and np.count_nonzero(regions) > np.count_nonzero(edges)
assert not np.any(edges[:10]) and not np.any(edges[38:])


def header(kind, width, height, stride, seq, ts, size=0, crc=0):
    return struct.pack('<4sHHIIIIIIQQIIQIIII', b'VRI1', 1, kind, 2, size,
                       width, height, stride, 10, seq, ts, 1, 1, 0, crc, width, height, 0)


with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    archive = tmp / 'input.vri'
    chunks = [header(1, 41, 35, 88, 0, 0)]
    for i in range(64):
        y, x = np.mgrid[:35, :41]
        values = (x*19+y*7+((x+i % 7)*y) % 41) % 1024
        padded = np.full((35, 44), 65535, dtype='<u2'); padded[:, :41] = values
        data = padded.tobytes()
        chunks.extend([header(2, 41, 35, 88, i, (i+1)*10**9, len(data), zlib.crc32(data)), data])
    chunks.append(header(3, 41, 35, 88, 64, 64*88*35))
    archive.write_bytes(b''.join(chunks))
    output = tmp / 'result.vrv'
    args = ['--input', str(archive), '--output', str(output), '--shader', shader,
            '--x', '3', '--y', '5', '--roi-width', '31', '--roi-height', '23',
            '--low', '.08', '--high', '.3', '--alpha', '6', '--filter', 'causal']
    run('vr-offline', args)
    payload = output.read_bytes(); frames = []; offset = 0
    for i in range(64):
        fields = struct.unpack_from('<4sIIIQQII', payload, offset)
        assert fields == (b'VRV1', 31, 23, 31*23*4, i, (i+1)*10**9, 1, 1)
        frames.append(np.frombuffer(payload, '<f4', 31*23, offset+40).copy())
        offset += 40+31*23*4
    assert offset == len(payload)
    reference = np.fromfile(build / 'motion-test.f32', '<f4').reshape(64, -1)
    assert np.max(np.abs(np.array(frames)-reference)) < 1e-5
    # Whole-frame default and alpha=0: every original pixel, no spatial decimation.
    full = tmp / 'full.vrv'
    run('vr-offline', ['--input', str(archive), '--output', str(full), '--shader', shader,
                       '--low', '.08', '--high', '.3', '--alpha', '0'])
    assert len(full.read_bytes()) == 64*(40+41*35*4)
    original = np.frombuffer(chunks[2], '<u2').reshape(35, 44)[:, :41]/1023
    first = np.frombuffer(full.read_bytes(), '<f4', 41*35, 40).reshape(35, 41)
    assert np.max(np.abs(first-original)) < 2e-6
    run('vr-offline', args, ok=False)  # must not overwrite
    for label, data in [('truncated', archive.read_bytes()[:-80]),
                        ('trailing', archive.read_bytes()+b'x'),
                        ('crc', archive.read_bytes()[:161]+b'\x7f'+archive.read_bytes()[162:])]:
        bad = tmp / (label+'.vri'); bad.write_bytes(data)
        bad_output = tmp / (label+'.vrv')
        bad_args = args.copy(); bad_args[1] = str(bad); bad_args[3] = str(bad_output)
        run('vr-offline', bad_args, ok=False)
        assert not bad_output.exists()
    outdir = tmp / 'features'
    run('vr-features', ['--input', str(archive), '--output-dir', str(outdir), '--shader', shader, '--every', '8'])
    records = [json.loads(line) for line in (outdir/'frames.jsonl').read_text().splitlines()]
    assert [r['sequence'] for r in records] == list(range(0,64,8))
    assert len(list(outdir.glob('*-regions.pgm'))) == 8
print('PASS: independent Sobel, focus masks, offline numerical parity, full frames, CRC/END/overwrite, feature bundle')
