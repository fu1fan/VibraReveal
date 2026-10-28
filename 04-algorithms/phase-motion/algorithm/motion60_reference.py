"""Frame-at-a-time port of ELF2 phasemag; no capture or display side effects."""
from dataclasses import dataclass
import math
import cv2
import numpy as np

EVEN = np.array([1, 4, 6, 4, 1], np.float32) / 16
ODD = np.array([-1, -2, 0, 2, 1], np.float32) / 8


@dataclass(frozen=True)
class Config:
    fps: float = 60.0
    lo: float = 3.0
    hi: float = 12.0
    alpha: float = 12.0
    levels: int = 3

    def validate(self):
        if not all(math.isfinite(x) for x in (self.fps, self.lo, self.hi, self.alpha)):
            raise ValueError("non-finite configuration")
        if not 0 < self.lo < self.hi < self.fps / 2:
            raise ValueError("require 0 < lo < hi < fps/2")
        if not 0 <= self.alpha <= 30 or self.levels != 3:
            raise ValueError("alpha must be 0..30; baseline uses three levels")

    def coefficients(self):
        self.validate()
        f0 = math.sqrt(self.lo * self.hi)
        q = max(.5, f0 / (self.hi - self.lo))
        w = 2 * math.pi * f0 / self.fps
        a = math.sin(w) / (2 * q)
        return np.array([a / (1+a), 0, -a / (1+a),
                         -2*math.cos(w)/(1+a), (1-a)/(1+a)], np.float32)


def spatial(img, kx, ky):
    return cv2.sepFilter2D(img, cv2.CV_32F, kx, ky,
                           borderType=cv2.BORDER_REFLECT_101)


def decompose(frame, levels):
    g = np.array(frame, dtype=np.float32, copy=True)
    bands = []
    for _ in range(levels):
        h, w = g.shape
        blur = spatial(g, EVEN, EVEN)
        g = cv2.resize(blur, (max(8, w//2), max(8, h//2)),
                       interpolation=cv2.INTER_AREA)
        bands.append(blur - cv2.resize(g, (w, h), interpolation=cv2.INTER_LINEAR))
    return bands, g


class Motion60:
    """Invalid frames/gaps discard state. Display frame skips must not call gap().

    Uses source direct form I initially to retain floating-point operation order.
    No normalization/contrast enhancement: black frames are legitimate input.
    """
    def __init__(self, config=Config()):
        config.validate()
        self.config = config
        self.coeff = config.coefficients()
        self.reset()

    def reset(self):
        self.states = []
        self.last_sequence = None
        self.shape = None

    def configure(self, config):
        config.validate()  # failed configuration must not destroy good state
        coeff = config.coefficients()
        self.config, self.coeff = config, coeff
        self.reset()

    def process(self, frame, sequence, valid=True):
        if not valid:
            self.reset()
            return None
        if frame.ndim != 2 or min(frame.shape) < 16 or not np.isfinite(frame).all():
            raise ValueError("expected finite 2D grayscale frame, minimum 16x16")
        if ((self.last_sequence is not None and sequence != self.last_sequence+1)
                or (self.shape is not None and self.shape != frame.shape)):
            self.reset()
        bands, cur = decompose(frame, self.config.levels)
        b0, b1, b2, a1, a2 = self.coeff
        fresh = not self.states
        output = []
        for i, b in enumerate(bands):
            rx, ry = spatial(b, ODD, EVEN), spatial(b, EVEN, ODD)
            r = np.sqrt(rx*rx + ry*ry) + np.float32(1e-6)
            phi = np.arctan2(b, r)
            if fresh:
                self.states.append(dict(phi0=phi.copy(), prev=phi.copy(),
                                        correction=np.zeros_like(phi),
                                        x1=np.zeros_like(phi), x2=np.zeros_like(phi),
                                        y1=np.zeros_like(phi), y2=np.zeros_like(phi)))
            st = self.states[i]
            delta = phi - st['prev']
            wrapped = (delta + np.pi) % (2*np.pi) - np.pi
            wrapped = np.where((wrapped == -np.pi) & (delta > 0), np.pi, wrapped)
            st['correction'] += np.where(np.abs(delta) < np.pi, 0, wrapped-delta)
            x = phi + st['correction'] - st['phi0']
            y = b0*x + b1*st['x1'] + b2*st['x2'] - a1*st['y1'] - a2*st['y2']
            angle = self.config.alpha*y
            output.append(b*np.cos(angle) + r*np.sin(angle))
            st.update(prev=phi, x2=st['x1'], x1=x, y2=st['y1'], y1=y)
        for band in reversed(output):
            h, w = band.shape
            cur = cv2.resize(cur, (w, h), interpolation=cv2.INTER_LINEAR) + band
        self.last_sequence, self.shape = sequence, frame.shape
        return cur
