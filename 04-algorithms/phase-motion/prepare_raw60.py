"""Byte-preserving temporal subsampling; no quantization, scaling or compression."""
import argparse, json, math
from pathlib import Path

FRAME_BYTES = 1280 * 800 * 2

def extract(source, output, source_fps=120., target_fps=60., start=0, count=None):
    source, output = Path(source), Path(output)
    if not all(math.isfinite(v) and v > 0 for v in (source_fps, target_fps)):
        raise ValueError('Positive finite frame rates required')
    ratio = source_fps / target_fps
    if ratio < 1 or abs(ratio-round(ratio)) > 1e-9:
        raise ValueError('Requires integer temporal stride; do not relabel FPS')
    stride = round(ratio)
    size = source.stat().st_size
    if size % FRAME_BYTES: raise ValueError('Incomplete RAW frame')
    total = size // FRAME_BYTES
    if start < 0 or start >= total: raise ValueError('Start out of range')
    available = (total-1-start)//stride+1
    count = available if count is None else count
    if count < 1 or count > available: raise ValueError('Count out of range')
    sidecar = output.with_suffix(output.suffix+'.json')
    if output.exists() or sidecar.exists(): raise FileExistsError('Refuse overwrite')
    with source.open('rb') as src, output.open('xb') as dst:
        for index in range(start, start+count*stride, stride):
            src.seek(index*FRAME_BYTES)
            frame = src.read(FRAME_BYTES)
            if len(frame) != FRAME_BYTES: raise EOFError('Source changed during read')
            dst.write(frame)
    info = dict(source=source.name, output=output.name, source_fps_assumed=source_fps,
                output_fps=target_fps, stride=stride, start_zero_based=start,
                output_frames=count, width=1280, height=800, dtype='uint16 LE RAW10',
                anti_alias_filter=False, exposure_equivalence=False)
    sidecar.write_text(json.dumps(info, indent=2)+'\n', encoding='utf-8')
    return info

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path); p.add_argument('output',type=Path)
    p.add_argument('--source-fps',type=float,default=120)
    p.add_argument('--target-fps',type=float,default=60)
    p.add_argument('--start',type=int,default=0); p.add_argument('--count',type=int)
    a=p.parse_args()
    print(json.dumps(extract(a.source,a.output,a.source_fps,a.target_fps,a.start,a.count),indent=2))
