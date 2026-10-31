#!/usr/bin/env python3
"""Independent NumPy spatial/causal reference for vr-motion-test output.
Run vr-motion-test first, then: python3 06-tests/vulkan_motion.py build/motion-test.f32
"""
import sys
import numpy as np


def blur(a, axis):
    pads = [(0, 0), (0, 0)]
    pads[axis] = (2, 2)
    p = np.pad(a, pads, mode='reflect' if a.shape[axis] > 1 else 'edge')
    result = np.zeros_like(a)
    for i, weight in enumerate([1/16, 4/16, 6/16, 4/16, 1/16]):
        s = [slice(None), slice(None)]
        s[axis] = slice(i, i+a.shape[axis])
        result += p[tuple(s)] * weight
    return result


def resize(a, shape):
    h, w = shape
    sh, sw = a.shape
    x = np.clip((np.arange(w)+.5)*sw/w-.5, 0, sw-1)
    y = np.clip((np.arange(h)+.5)*sh/h-.5, 0, sh-1)
    x0, y0 = x.astype(int), y.astype(int)
    x1, y1 = np.minimum(x0+1, sw-1), np.minimum(y0+1, sh-1)
    fx, fy = (x-x0)[None, :], (y-y0)[:, None]
    return ((a[y0[:, None], x0]*(1-fx)+a[y0[:, None], x1]*fx)*(1-fy)
            +(a[y1[:, None], x0]*(1-fx)+a[y1[:, None], x1]*fx)*fy)


def reference():
    histories = [None]*3
    even = [1/16, 4/16, 6/16, 4/16, 1/16]
    odd = [-1/8, -2/8, 0, 2/8, 1/8]
    f0 = np.sqrt(.08*.3)
    q = max(.5, f0/(.3-.08))
    omega = 2*np.pi*f0
    a = np.sin(omega)/(2*q)
    b0, b2, a1, a2 = a/(1+a), -a/(1+a), -2*np.cos(omega)/(1+a), (1-a)/(1+a)
    outputs = []
    yy, xx = np.mgrid[:35, :41]
    for seq in range(64):
        current = ((xx*19+yy*7+((xx+seq%7)*yy)%41)%1024/1023.)[5:28, 3:34]
        amplified = []
        for level in range(3):
            h, w = current.shape
            small = resize(blur(blur(current, 1), 0), (max(1, h//2), max(1, w//2)))
            band = current-resize(small, current.shape)
            p = np.pad(band, 2, mode='reflect')
            rx, ry = np.zeros_like(band), np.zeros_like(band)
            for y in range(5):
                for x in range(5):
                    rx += p[y:y+h, x:x+w]*odd[x]*even[y]
                    ry += p[y:y+h, x:x+w]*even[x]*odd[y]
            r = np.sqrt(rx*rx+ry*ry)+1e-6
            phi = np.arctan2(band, r)
            if histories[level] is None:
                histories[level] = [phi.copy(), phi.copy()] + [np.zeros_like(phi) for _ in range(5)]
            origin, previous, correction, x1, x2, y1, y2 = histories[level]
            delta = phi-previous
            wrapped = delta-2*np.pi*np.floor((delta+np.pi)/(2*np.pi))
            correction = correction+np.where(np.abs(delta)>=np.pi, wrapped-delta, 0)
            x = phi+correction-origin
            y = b0*x+b2*x2-a1*y1-a2*y2
            amplified.append(band*np.cos(6*y)+r*np.sin(6*y))
            histories[level] = [origin, phi, correction, x, x1, y, y1]
            current = small
        for band in reversed(amplified):
            current = band+resize(current, band.shape)
        outputs.append(current)
    return np.array(outputs)


actual = np.fromfile(sys.argv[1], dtype='<f4').reshape(64, 23, 31)
expected = reference()
error = float(np.max(np.abs(actual-expected)))
assert np.isfinite(actual).all()
assert error < 2e-4, f'Vulkan/reference mismatch: {error}'
yy, xx = np.mgrid[:35, :41]
original = ((xx*19+yy*7+((xx+10%7)*yy)%41)%1024/1023.)[5:28, 3:34]
assert np.max(np.abs(expected[10]-original)) > 1e-3, 'Test must exercise phase amplification'
print(f'PASS: 64 frames / 3 levels / nonzero phase amplification; max absolute error={error:.8g}')
