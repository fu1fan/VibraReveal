#!/usr/bin/env python3
"""CLI framing, argument rejection and blocked-output tests (Linux Vulkan ICD).
Usage: python3 06-tests/vulkan_motion_cli.py /path/vr-motion /path/motion.comp.spv
"""
import json
import struct
import subprocess
import sys

binary, shader = sys.argv[1:]
base = [binary, '--source', 'synthetic', '--width', '32', '--height', '32',
        '--roi-width', '31', '--roi-height', '23', '--fps', '2', '--low', '.1',
        '--high', '.5', '--frames', '3', '--shader', shader]

def events(stderr):
    # Vendor ICD diagnostics may coexist with application JSON on stderr.
    return [json.loads(s) for s in stderr.decode().splitlines() if s.startswith('{')]

result = subprocess.run(base + ['--output', '-'], capture_output=True, timeout=10)
assert result.returncode == 0, result.stderr
payload = result.stdout
offset = 0
for i in range(3):
    magic, w, h, size, seq, ts, num, den = struct.unpack_from('<4sIIIQQII', payload, offset)
    assert (magic, w, h, size, seq, num, den) == (b'VRV1', 31, 23, 31*23*4, i, 2000000, 1000000)
    assert ts == i*500000000+1
    offset += 40+size
assert offset == len(payload), 'Extra driver diagnostics in binary output'
assert events(result.stderr)[-1] == {'event': 'complete', 'frames': 3}
for args in [['--fps', 'nan'], ['--roi-width', '0'], ['--low', '15'], ['--unknown', '1']]:
    result = subprocess.run([binary]+args, capture_output=True, timeout=5)
    assert result.returncode != 0
    assert events(result.stderr)[-1]['event'] == 'error'
# Never read stdout until process exits: > pipe capacity forces output timeout.
blocked = [binary, '--source', 'synthetic', '--width', '256', '--height', '256',
           '--roi-width', '256', '--roi-height', '256', '--fps', '2', '--low', '.1',
           '--high', '.5', '--frames', '3', '--shader', shader, '--output', '-']
p = subprocess.Popen(blocked, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
try:
    code = p.wait(timeout=10)
    _, stderr = p.communicate()
except BaseException:
    p.kill(); p.communicate()
    raise
assert code == 2, stderr
assert events(stderr)[-1]['code'] == 'OUTPUT_DEADLINE_EXCEEDED', stderr
print('PASS: binary stdout, frame headers, argument errors, blocked-output deadline')
