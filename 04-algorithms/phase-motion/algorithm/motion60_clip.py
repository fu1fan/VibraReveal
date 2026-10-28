"""Bounded-memory temporal stage for the original whole-clip FFT mask.

This module is an offline oracle/preparation stage, not the deployed clip path.
Input is already unwrapped phase relative to the first frame. No zero padding,
windowing, or causal approximation is introduced.
"""
import numpy as np
import cv2
from algorithm.motion60_reference import decompose, spatial, EVEN, ODD


def temporal_kernel(frames, fps, lo, hi):
    if not isinstance(frames, (int, np.integer)) or not 30 <= frames <= 120:
        raise ValueError('clip must contain 30..120 complete frames')
    if not all(np.isfinite(v) for v in (fps, lo, hi)) or not 0 < lo < hi < fps / 2:
        raise ValueError('expected 0 < lo < hi < fps/2')
    frequencies = np.fft.rfftfreq(frames, 1.0 / fps)
    mask = ((frequencies >= lo) & (frequencies <= hi)).astype(np.float64)
    # Circular convolution is exactly rfft -> mask -> irfft at this length.
    return np.fft.irfft(mask, n=frames).astype(np.float32)


def filter_phase_tile(phase, kernel):
    phase = np.asarray(phase, dtype=np.float32)
    kernel = np.asarray(kernel, dtype=np.float32)
    if phase.ndim < 2 or not 30 <= phase.shape[0] <= 120 or kernel.shape != (phase.shape[0],):
        raise ValueError('phase is chronological [frames, ...], matching kernel')
    if not np.isfinite(phase).all() or not np.isfinite(kernel).all():
        raise ValueError('nonfinite phase or kernel')
    result = np.zeros_like(phase)
    # Deliberately float32 accumulation to model the planned HLS arithmetic.
    for t in range(phase.shape[0]):
        for source in range(phase.shape[0]):
            result[t] += kernel[(t - source) % phase.shape[0]] * phase[source]
    return result


def process_clip_two_pass(raw, fps, lo, hi, alpha, tile_pixels=4096):
    """Oracle for the DDR schedule: phase pass, temporal tiles, reconstruct pass.

    Uses arrays here; the board schedule stores phase in DDR and retains only
    one frame's spatial intermediates in work memory. Returns float pixels.
    """
    raw = np.asarray(raw)
    if raw.ndim != 3 or min(raw.shape[1:]) < 64 or any(d % 8 for d in raw.shape[1:]):
        raise ValueError('require frames x height x width, >=64, divisible by 8')
    if not np.isfinite(raw).all() or not np.isfinite(alpha) or not 0 <= alpha <= 30 or tile_pixels < 1:
        raise ValueError('invalid image, gain, or tile size')
    count, height, width = raw.shape
    kernel = temporal_kernel(count, fps, lo, hi)
    phases = [np.empty((count, height >> level, width >> level), np.float32) for level in range(3)]
    for t in range(count):
        bands, _ = decompose(raw[t], 3)
        for level, band in enumerate(bands):
            rx, ry = spatial(band, ODD, EVEN), spatial(band, EVEN, ODD)
            radius = np.sqrt(rx * rx + ry * ry) + np.float32(1e-6)
            phases[level][t] = np.arctan2(band, radius)
    matrix = kernel[(np.arange(count)[:, None] - np.arange(count)[None, :]) % count]
    for phase in phases:
        flat = phase.reshape(count, -1)
        for start in range(0, flat.shape[1], tile_pixels):
            tile = np.unwrap(flat[:, start:start + tile_pixels], axis=0)
            tile = tile - tile[:1]
            flat[:, start:start + tile_pixels] = matrix @ tile
    output = np.empty(raw.shape, np.float32)
    for t in range(count):
        bands, current = decompose(raw[t], 3)
        for level in reversed(range(3)):
            band = bands[level]
            rx, ry = spatial(band, ODD, EVEN), spatial(band, EVEN, ODD)
            radius = np.sqrt(rx * rx + ry * ry) + np.float32(1e-6)
            delta = np.float32(alpha) * phases[level][t]
            rotated = band * np.cos(delta) + radius * np.sin(delta)
            current = rotated + cv2.resize(current, (band.shape[1], band.shape[0]), interpolation=cv2.INTER_LINEAR)
        output[t] = current
    return output
