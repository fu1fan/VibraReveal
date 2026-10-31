#!/usr/bin/env python3
"""Real Bulk->Metal run, clock calibration and stage/drop accounting.

Board clock probe must be built by CNB and deployed over Wi-Fi. Stop the camera
service before running. End-to-end estimates begin at the V4L2 MONOTONIC timestamp;
they do not measure exposure start, monitor presentation or filter phase delay.
"""
import argparse
import bisect
import json
from pathlib import Path
import select
import shlex
import statistics
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--board', required=True)
p.add_argument('--ssh-control')
p.add_argument('--binary', required=True)
p.add_argument('--frames', type=int, default=36000)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--write-output', action='store_true', help='Serialize Float32 output to /dev/null instead of discarding it')
a=p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
ssh=['ssh']+(['-S',a.ssh_control] if a.ssh_control else [])+[a.board]
probe=subprocess.Popen(ssh+['/usr/local/bin/vr-clock-probe'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
calibrations=[]


def calibrate(count=40):
    samples=[]
    for _ in range(count):
        t0=time.monotonic()
        probe.stdin.write(b'clock\n')
        if not select.select([probe.stdout],[],[],5)[0]:
            raise RuntimeError('Wi-Fi clock probe timed out')
        remote=int(probe.stdout.readline())/1e9
        t1=time.monotonic()
        # Board timestamp occurs between local send and receive: causal offset bounds.
        samples.append({'host':(t0+t1)/2,'offset':remote-(t0+t1)/2,'halfRTT':(t1-t0)/2,
                        'offsetLower':remote-t1,'offsetUpper':remote-t0})
        time.sleep(.005)
    best=min(samples,key=lambda s:s['halfRTT'])
    calibrations.append(best)
    (a.output/'clocks.json').write_text(json.dumps({'best':calibrations,'latestSamples':samples},indent=2)+'\n')
    print('clock half RTT ms',round(best['halfRTT']*1000,3),flush=True)


def quantiles(values):
    v=sorted(values)
    def q(f):
        index=(len(v)-1)*f; lo=int(index); hi=min(lo+1,len(v)-1)
        return v[lo]+(v[hi]-v[lo])*(index-lo)
    return {'min':v[0],'p50':q(.5),'p95':q(.95),'p99':q(.99),'max':v[-1],'mean':statistics.mean(v)}


sender=receiver=None
try:
    calibrate()
    with (a.output/'sender.log').open('w') as sl, (a.output/'receiver.log').open('w') as rl, open('/dev/null','wb') as sink:
        sender=subprocess.Popen(ssh+[shlex.join(['/usr/local/sbin/vr-usb-stream','--source','camera','--mode','realtime',
            '--frames',str(a.frames),'--timeout','30'])],stdout=sl,stderr=subprocess.STDOUT)
        receiver=subprocess.Popen([a.binary,'stream','--usb','true','--adaptive','false','--alpha','10','--levels','3',
            '--low','3','--high','12','--telemetry',str(a.output/'frames.jsonl'),
            '--discard-output','false' if a.write_output else 'true'],stdout=sink,stderr=rl)
        deadline=time.monotonic()+a.frames/100+45
        while receiver.poll() is None:
            try:
                receiver.wait(timeout=20)
            except subprocess.TimeoutExpired:
                if time.monotonic()>deadline: raise RuntimeError('stream timed out')
                calibrate(20)
        sc=sender.wait(timeout=35)
        assert receiver.returncode==sc==0, (receiver.returncode,sc)
    calibrate()
finally:
    if sender is not None and sender.poll() is None:
        # Close the owned FunctionFS session as well as the local SSH process.
        subprocess.run(ssh+['pkill -TERM -f "^/bin/sh /usr/local/sbin/vr-usb-stream "'],
                       capture_output=True, timeout=10, check=False)
    for process in (receiver,sender):
        if process is not None and process.poll() is None:
            process.terminate(); process.wait(timeout=10)
    probe.stdin.close()
    probe.wait(timeout=10)

rows=[json.loads(s) for s in (a.output/'frames.jsonl').read_text().splitlines()]
received=[s for s in rows if s['event']=='received']
processed=[s for s in rows if s['event']=='processed']
dropped=[s['sequence'] for s in rows if s['event']=='dropped']
summary=next(s for s in rows if s['event']=='summary')
assert len(received)==a.frames==summary['submitted']
assert len(processed)==summary['completed'] and len(dropped)==summary['dropped']
assert len(processed)+len(dropped)==len(received) and summary['failed']==0
assert set(s['sequence'] for s in processed).isdisjoint(dropped)
assert set(s['sequence'] for s in received)==set(range(a.frames))
assert all((s['width'],s['height'])==(1280,800) for s in received+processed)
assert summary['transport']['sequenceGaps']==summary['transport']['senderQueueDropped']==0
hosts=[c['host'] for c in calibrations]


def clock(host):
    i=max(0,min(len(hosts)-2,bisect.bisect_right(hosts,host)-1))
    first,last=calibrations[i:i+2]
    t=max(0,min(1,(host-first['host'])/(last['host']-first['host'])))
    return (first['offset']*(1-t)+last['offset']*t, first['halfRTT']*(1-t)+last['halfRTT']*t)


def metrics(frames):
    total=[]; transfer=[]; uncertainties=[]
    for f in frames:
        offset,uncertainty=clock(f['completed'])
        total.append((f['completed']+offset-f['capture'])*1000)
        transfer.append((f['received']+offset-f['capture'])*1000)
        uncertainties.append(uncertainty*1000)
    return {'estimatedCaptureToResultMS':quantiles(total),'estimatedCaptureToReceiveMS':quantiles(transfer),
            'estimatedCaptureToSinkMS':quantiles([t+f['sinkSeconds']*1000 for t,f in zip(total,frames)]),
            'receiveToResultMS':quantiles([(f['completed']-f['received'])*1000 for f in frames]),
            'processingMS':quantiles([f['processingSeconds']*1000 for f in frames]),
            'queueAndDispatchMS':quantiles([max(0,f['completed']-f['received']-f['processingSeconds'])*1000 for f in frames]),
            'sinkMS':quantiles([f['sinkSeconds']*1000 for f in frames]),
            'clockHalfRTTMS':quantiles(uncertainties)}

span=received[-1]['capture']-received[0]['capture']
result={'framesRequested':a.frames,'settings':{'width':1280,'height':800,'fps':120,'alpha':10,'levels':3,
            'lowHz':3,'highHz':12,'adaptive':False,'sink':'Float32 serialization to /dev/null' if a.write_output else 'discard after Float32 readback'},
        'summary':summary,'captureFPS':(a.frames-1)/span,
        'completedPerCaptureSecond':len(processed)/(span+1/120),
        'outputCadenceFPS':(len(processed)-1)/(processed[-1]['completed']-processed[0]['completed']),
        'algorithmDropPercent':len(dropped)/a.frames*100,
        'droppedSequences':dropped,
        'dropsInFirst120SourceFrames':sum(s<120 for s in dropped),
        'dropsAfterFirst120SourceFrames':sum(s>=120 for s in dropped),
        'historyResets':sum(s['historyReset'] for s in processed),'allFrames':metrics(processed),
        'steadyAfterFirst120SourceFrames':metrics([s for s in processed if s['sequence']>=120]),
        'clockNote':'Piecewise linear offset estimate using minimum RTT samples over Wi-Fi; half RTT is calibration uncertainty, not a proven bound between samples. Excludes exposure and display timing.',
        'clockOffsetChangeMS':(calibrations[-1]['offset']-calibrations[0]['offset'])*1000}
(a.output/'report.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2),flush=True)
