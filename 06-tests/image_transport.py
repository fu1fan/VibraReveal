#!/usr/bin/env python3
"""Cross-language transport acceptance, including a deliberately slow receiver.

Usage: image_transport.py /path/to/vr-stream /path/to/vr-receive /path/to/vibrareveal
Runs on macOS with a native vr-stream build; ARM64 is built separately on CNB.
"""
import json
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

SENDER, RECEIVER, BACKEND = map(str, sys.argv[1:4])


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def start(*args):
    p = port()
    process = subprocess.Popen([SENDER, '--port', str(p), *map(str, args)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    # Read the readiness line without opening the single-client listening socket.
    line = process.stderr.readline()
    assert b'listening=' in line, line
    return process, p


def receive(p, *args, ok=True):
    result = subprocess.run([RECEIVER, '--host', '127.0.0.1', '--port', str(p), *map(str, args)],
                            capture_output=True, timeout=30)
    assert (result.returncode == 0) == ok, result.stderr
    return json.loads(result.stdout) if ok else result


def finish(process, ok=True):
    _, err = process.communicate(timeout=20)
    assert (process.returncode == 0) == ok, err
    return err


def readn(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count-len(data))
        if not chunk:
            raise EOFError('truncated stream')
        data.extend(chunk)
    return bytes(data)


def packets(path):
    raw = path.read_bytes()
    pos = 0
    while pos < len(raw):
        h = raw[pos:pos+80]
        assert len(h) == 80 and h[:4] == b'VRI1'
        size = struct.unpack_from('<I', h, 12)[0]
        data = raw[pos+80:pos+80+size]
        assert len(data) == size
        if size:
            assert zlib.crc32(data) == struct.unpack_from('<I', h, 64)[0]
        yield h, data
        pos += 80+size


with tempfile.TemporaryDirectory(prefix='vr-transport-tests-') as temporary:
    root = Path(temporary)
    # Odd dimensions, row padding, fractional sampling and real supplied timestamps.
    w, h, stride, count, fps = 33, 17, 72, 48, 119.88
    payload = bytes((i*17+3) % 256 for i in range(stride*h*count))
    (root/'input.raw').write_bytes(payload)
    times = [(100+i, 1_000_000_000+round(i/fps*1e9)) for i in range(count)]
    (root/'input.idx').write_bytes(b''.join(struct.pack('<QQ', *t) for t in times))
    archive = root/'capture.vri'
    process, p = start('--mode', 'offline', '--source', 'file', '--input', root/'input.raw',
                       '--index', root/'input.idx', '--width', w, '--height', h, '--stride', stride,
                       '--bits', 16, '--fps', fps, '--frames', 0)
    report = receive(p, '--output', archive)
    finish(process)
    assert report['frames'] == count and report['sequenceGaps'] == 0 and not report['syntheticTimestamps']
    frames = [(header, data) for header, data in packets(archive) if data]
    assert b''.join(data for _, data in frames) == payload
    assert [struct.unpack_from('<QQ', header, 32) for header, _ in frames] == times
    result = subprocess.run([BACKEND, 'offline', '--archive', str(archive), '--low', '3', '--high', '12', '--alpha', '0'],
                            capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr
    pos = 0
    output_frames = []
    while pos < len(result.stdout):
        size = struct.unpack_from('<I', result.stdout, pos)[0]
        metadata = json.loads(result.stdout[pos+4:pos+4+size])
        output_frames.append(metadata)
        pos += 4+size+metadata['width']*metadata['height']*4
    assert len(output_frames) == count and output_frames[0]['sequence'] == 100
    assert abs(output_frames[-1]['timestamp']-times[-1][1]/1e9) < 1e-9

    # Real-time replay follows a non-integer source rate, not available bandwidth.
    process, p = start('--mode', 'realtime', '--source', 'synthetic', '--width', 65, '--height', 49,
                       '--frames', 20, '--fps', '59.94')
    report = receive(p)
    finish(process)
    assert report['frames'] == 20 and report['sequenceGaps'] == 0, report
    assert 0.25 < report['seconds'] < 3, report

    # ACK delay must trigger bounded-queue drops and spatial adaptation, not
    # a growing in-memory queue or a silent lower sampling-rate claim.
    process, p = start('--mode', 'realtime', '--source', 'synthetic', '--width', 256, '--height', 192,
                       '--frames', 100, '--fps', 120)
    with socket.create_connection(('127.0.0.1', p), timeout=5) as sock:
        hello = readn(sock, 80)
        widths, drops = [], []
        while True:
            header = readn(sock, 80)
            kind = struct.unpack_from('<H', header, 6)[0]
            if kind == 3:
                sock.sendall(b'VRDONE01')
                break
            data = readn(sock, struct.unpack_from('<I', header, 12)[0])
            assert zlib.crc32(data) == struct.unpack_from('<I', header, 64)[0]
            widths.append(struct.unpack_from('<I', header, 16)[0])
            drops.append(struct.unpack_from('<Q', header, 56)[0])
            time.sleep(0.025)
            sock.sendall(header[32:40])
        assert min(widths) < max(widths) and drops[-1] > 0, (widths, drops)
    finish(process)

    # Interrupted file and interrupted TCP must never get a success end marker.
    (root/'short.raw').write_bytes(b'x' * 15)
    process, p = start('--mode', 'offline', '--source', 'file', '--input', root/'short.raw',
                       '--width', 4, '--height', 2, '--frames', 0)
    receive(p, ok=False); finish(process, ok=False)
    process, p = start('--mode', 'realtime', '--source', 'synthetic', '--frames', 10000)
    with socket.create_connection(('127.0.0.1', p)) as sock:
        readn(sock, 80)
    finish(process, ok=False)

    # Corruption, truncation and a wrong end count are independently rejected.
    original = archive.read_bytes()
    corrupt = bytearray(original); corrupt[160] ^= 1
    for name, data in [('corrupt', corrupt), ('truncated', original[:-1]), ('trailing', original+b'x')]:
        path = root/name; path.write_bytes(data)
        result = subprocess.run([RECEIVER, '--archive', str(path)], capture_output=True, timeout=5)
        assert result.returncode != 0, name

print('PASS: exact file/index transfer, offline backend, fractional pacing, bounded overload, disconnect and corruption')
