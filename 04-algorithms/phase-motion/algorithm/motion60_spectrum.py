"""Streaming oracle for the ELF2 phase-correlation spectrum, not a PL implementation.

Keep the first valid ROI as the phase reference. Detrending belongs to each
completed spectrum window, not to individual displacements. A capture gap or
invalid frame discards history; it must never masquerade as a uniform sample.
"""
from collections import deque
from dataclasses import dataclass
import math
import numpy as np


@dataclass(frozen=True)
class SpectrumConfig:
    fps: float = 60.379777
    roi: tuple = (512, 272, 256, 256)
    samples: int = 256
    refresh_hz: float = 5.0
    # Diagnostic validity only; no change to the original displacement math.
    minimum_variance_dn2: float = 1.0

    def validate(self):
        if not (math.isfinite(self.fps) and self.fps > 0 and
                math.isfinite(self.refresh_hz) and 0 < self.refresh_hz <= self.fps):
            raise ValueError('invalid sampling or refresh rate')
        if not isinstance(self.samples, int) or self.samples < 4:
            raise ValueError('window must contain at least four samples')
        if len(self.roi) != 4 or any(not isinstance(x, int) for x in self.roi):
            raise ValueError('ROI requires four integers')
        x, y, w, h = self.roi
        if x < 0 or y < 0 or w < 4 or h < 4:
            raise ValueError('invalid ROI')
        if not math.isfinite(self.minimum_variance_dn2) or self.minimum_variance_dn2 < 0:
            raise ValueError('invalid texture threshold')


def linear_detrend(signal):
    """Least-squares line removal, algebraically equivalent to np.polyfit(..., 1)."""
    d = np.asarray(signal, dtype=np.float64)
    t = np.arange(d.shape[0], dtype=np.float64)
    t -= t.mean()
    centered = d - d.mean(axis=0)
    slope = np.sum(t[:, None] * centered, axis=0) / np.dot(t, t)
    return centered - t[:, None] * slope


def displacement(reference_fft, current_fft):
    cross = reference_fft * np.conj(current_fft)
    cross /= np.abs(cross) + 1e-9
    correlation = np.fft.ifft2(cross).real
    iy, ix = np.unravel_index(np.argmax(correlation), correlation.shape)
    h, w = correlation.shape

    def sub(p, c, n):
        denominator = p - 2 * c + n
        return .5 * (p - n) / denominator if abs(denominator) > 1e-12 else 0.0

    center = correlation[iy, ix]
    sx = ix + sub(correlation[iy, (ix-1) % w], center, correlation[iy, (ix+1) % w])
    sy = iy + sub(correlation[(iy-1) % h, ix], center, correlation[(iy+1) % h, ix])
    # Preserve ELF2's sign and wrap convention, including negative subpixel peaks.
    return (float(sx-w if sx > w/2 else sx),
            float(sy-h if sy > h/2 else sy)), float(center)


class MotionSpectrum:
    def __init__(self, config=SpectrumConfig()):
        self.resets = 0
        self.configure(config)

    def configure(self, config):
        config.validate()
        self.config = config
        _, _, w, h = config.roi
        self.spatial_window = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
        self.reset('configuration')

    def reset(self, reason):
        self.reference_fft = None
        self.last_sequence = None
        self.history = deque(maxlen=self.config.samples)
        self.next_spectrum_sequence = None
        self.last_reset_reason = reason
        self.resets += 1

    def push(self, frame, sequence, valid=True):
        if not isinstance(sequence, int) or sequence < 0:
            raise ValueError('sequence must be a nonnegative integer')
        if not valid:
            self.reset('invalid capture frame')
            return {'valid': False, 'reason': self.last_reset_reason, 'spectrum': None}
        x, y, w, h = self.config.roi
        frame = np.asarray(frame)
        if frame.ndim != 2 or y+h > frame.shape[0] or x+w > frame.shape[1]:
            raise ValueError('ROI outside frame')
        roi = frame[y:y+h, x:x+w].astype(np.float32)
        if not np.isfinite(roi).all():
            raise ValueError('nonfinite image')
        if self.last_sequence is not None and sequence != self.last_sequence + 1:
            self.reset('capture sequence gap')
        variance = float(np.var(roi, dtype=np.float64))
        if variance <= self.config.minimum_variance_dn2:
            self.reset('insufficient texture')
            return {'valid': False, 'reason': self.last_reset_reason,
                    'variance_dn2': variance, 'spectrum': None}
        current_fft = np.fft.fft2(roi * self.spatial_window)
        if self.reference_fft is None:
            self.reference_fft = current_fft.copy()
        shift, peak = displacement(self.reference_fft, current_fft)
        self.last_sequence = sequence
        self.history.append(shift)
        result = dict(valid=True, sequence=sequence, dx_px=shift[0], dy_px=shift[1],
                      correlation_peak=peak, variance_dn2=variance,
                      samples_collected=len(self.history), spectrum=None)
        # Sample every valid frame; throttle only spectrum publication.
        interval = self.config.fps / self.config.refresh_hz
        if len(self.history) == self.config.samples and (
                self.next_spectrum_sequence is None or
                sequence >= self.next_spectrum_sequence):
            data = linear_detrend(np.asarray(self.history))
            window = np.hanning(self.config.samples)
            amplitude = np.abs(np.fft.rfft(data*window[:, None], axis=0))*2/window.sum()
            result['spectrum'] = dict(
                frequency_hz=np.fft.rfftfreq(self.config.samples, 1/self.config.fps),
                amplitude_px=amplitude, detrended_px=data,
                first_sequence=sequence-self.config.samples+1, last_sequence=sequence)
            self.next_spectrum_sequence = (sequence + interval if self.next_spectrum_sequence is None
                                           else self.next_spectrum_sequence + interval)
        return result
