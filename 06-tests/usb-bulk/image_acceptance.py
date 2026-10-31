#!/usr/bin/env python3
"""Run against the actual board over Wi-Fi, with the Bulk service stopped.

Example: image_acceptance.py --board root@192.168.8.204 --receiver /path/vr-receive
  --ssh-control /tmp/vr-bulk-ssh --offline-raw /tmp/recording.raw --frames 36000
No board compilation or TCP image transfer is performed here.
"""
import argparse
import json
from pathlib import Path
import shlex
import signal
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--board', required=True)
p.add_argument('--receiver', required=True)
p.add_argument('--ssh-control')
p.add_argument('--offline-raw')
p.add_argument('--frames', type=int, default=7200)
p.add_argument('--skip-long', action='store_true')
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
ssh = ['ssh'] + (['-S', a.ssh_control] if a.ssh_control else []) + [a.board]
results = {}


def session(name, options, pause=0, expect_success=True):
    with (a.output / (name + '.sender.log')).open('w') as sender_log, \
            (a.output / (name + '.json')).open('w') as receiver_log, \
            (a.output / (name + '.stderr')).open('w') as receiver_err:
        sender = subprocess.Popen(ssh + [shlex.join(['/usr/local/sbin/vr-usb-stream', '--timeout', '10', *options])],
                                  stdout=sender_log, stderr=subprocess.STDOUT)
        receiver = subprocess.Popen([a.receiver, '--usb', '--timeout', '15'], stdout=receiver_log, stderr=receiver_err)
        try:
            if pause:
                # USB enumeration time varies between sessions. Wait for the camera
                # FD (opened only after VRSTART1), not a fixed delay from process start.
                deadline = time.monotonic() + 20
                while True:
                    probe = subprocess.run(ssh + ['p=$(pidof vr-stream); for f in /proc/$p/fd/*; do readlink "$f"; done'],
                                           capture_output=True, text=True, timeout=5)
                    if '/dev/video11' in probe.stdout:
                        break
                    if time.monotonic() > deadline:
                        raise RuntimeError('camera did not start before pause test')
                    time.sleep(0.1)
                time.sleep(0.5)
                receiver.send_signal(signal.SIGSTOP)
                time.sleep(pause)
                receiver.send_signal(signal.SIGCONT)
            rc = receiver.wait(timeout=max(40, a.frames / 100 + 30))
            sc = sender.wait(timeout=20)
        finally:
            if receiver.poll() is None:
                receiver.send_signal(signal.SIGCONT)
                receiver.kill()
                receiver.wait()
            if sender.poll() is None:
                # Killing SSH alone may leave its remote child; ask the owning launcher to clean up.
                subprocess.run(ssh + ['pkill -TERM -f "^/bin/sh /usr/local/sbin/vr-usb-stream "'], check=False)
                sender.terminate()
                sender.wait(timeout=20)
    result = {'sender_exit': sc, 'receiver_exit': rc, 'receiver_pause_seconds': pause}
    if expect_success:
        assert rc == sc == 0, (name, result)
        result['receiver'] = json.loads((a.output / (name + '.json')).read_text())
        stats = result['receiver']
        assert stats['sequenceGaps'] == stats['senderQueueDropped'] == 0
        assert stats['payloadBytes'] == stats['frames'] * 1280 * 800 * 2
        if options[options.index('--source') + 1] == 'camera':
            expected = int(options[options.index('--frames') + 1])
            assert stats['frames'] == expected
            assert stats['firstSequence'] == 0 and stats['lastSequence'] == expected - 1
            capture_fps = (expected - 1) * 1e9 / (stats['lastTimestampNS'] - stats['firstTimestampNS'])
            assert 119 <= capture_fps <= 122, capture_fps
        lines = (a.output / (name + '.sender.log')).read_text().splitlines()
        result['sender'] = json.loads(next(line for line in lines if line.startswith('{')))
    else:
        assert rc != 0 and sc != 0, (name, result)
        assert 'Capture ring overrun' in (a.output / (name + '.sender.log')).read_text()
    results[name] = result
    (a.output / 'summary.json').write_text(json.dumps(results, indent=2) + '\n')
    print(name, json.dumps(result), flush=True)


camera = ['--source', 'camera', '--mode', 'realtime']
if not a.skip_long:
    session('realtime', camera + ['--frames', str(a.frames)])
session('pause-100ms', camera + ['--frames', '600'], pause=0.1)
session('overrun-800ms', camera + ['--frames', '600'], pause=0.8, expect_success=False)
# Successful reconnect immediately after the failed session is part of acceptance.
session('reconnect', camera + ['--frames', '240'])
if a.offline_raw:
    for i in range(3):
        session('offline-' + str(i+1), ['--source', 'file', '--mode', 'offline', '--input', a.offline_raw,
                                       '--frames', '0', '--fps', '120'])
