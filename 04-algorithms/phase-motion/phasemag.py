#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
phasemag.py — 相位式运动放大(Phase-based Video Motion Magnification)离线验证工具
================================================================================
用途: 用真实拍摄数据在 PC 上验证运动放大效果(FPGA 比赛 W1-2 阶段), 并输出:
      ① 放大后视频(左原图|右放大) ② 真实位移曲线+频谱(亚像素相位相关)
      ③ 静止段噪声底线对比余量 ④ 定点化误差评估(给 FPGA 定位宽)

算法: 拉普拉斯金字塔 + Riesz(单演信号)局部相位 + 时域带通 + 相位放大 + 重建
      (Wadhwa et al. SIGGRAPH 2013 的实用化变体; 全部使用 5-tap 可分离核,
       与 FPGA 定点实现逐操作对应, 可直接当 golden model)

依赖: numpy, opencv-python   (matplotlib 可选, 缺失时只出 CSV)
安装: pip install numpy opencv-python matplotlib

采集规范(重要):
  - 目标: 音箱纸盆/悬臂梁等 <2px 的周期性振动, 驱动 5~15Hz 正弦
  - 机位刚性固定; DC-LED 照明(避开市电频闪!); 锁定曝光/增益/对焦; 关防抖
  - 拍 10~20s, 最好 60fps; 另拍一段"目标静止"的同景别视频做噪声底线
  - 手机是卷帘快门+降噪+压缩, 效果不如 raw 数据客观; 有条件用 OV9281 抓
    raw_dump 序列(每帧 2048000 字节 uint16 LE)喂给本工具最准

用法:
  python phasemag.py --selftest                        # ① 必先跑: 合成数据自检
  python phasemag.py --input speaker.mp4 --lo 5 --hi 12 --alpha 12
  python phasemag.py --input speaker.mp4 --baseline idle.mp4      # 带噪声底线
  python phasemag.py --input rawdir/ --fps 30         # raw_dump 帧目录(*.bin)
  python phasemag.py --input speaker.mp4 --fx 12      # 12bit 定点化误差评估

输出(--outdir 默认 phasemag_out/):
  mag_sbs.mp4  逐帧 左=原始|右=放大     motion.csv   位移时间序列
  report.png   位移曲线+频谱            report.txt   指标与定点定标建议
"""

import argparse
import glob
import math
import os
import sys

import numpy as np

try:
    import cv2
except ImportError:
    sys.exit("缺少 opencv-python, 请先:  pip install opencv-python")

# ---- 5-tap 可分离核(与 FPGA 定点实现对应) ----
K_EVEN = np.array([1, 4, 6, 4, 1], np.float32) / 16.0   # 平滑/近似Hilbert偶对称
K_ODD = np.array([-1, -2, 0, 2, 1], np.float32) / 8.0   # 奇对称(导数类)


# ============================== 基础算子 ==============================
def sep2d(img, kx, ky):
    return cv2.sepFilter2D(img, cv2.CV_32F, kx, ky,
                           borderType=cv2.BORDER_REFLECT_101)


def blur_stack(G):
    out = np.empty_like(G)
    for t in range(G.shape[0]):
        out[t] = sep2d(G[t], K_EVEN, K_EVEN)
    return out


def build_bands(stack, levels):
    """拉普拉斯金字塔: 返回 [B0,B1,..] 各带(全帧时域栈) + 最粗低通残差"""
    T, H, W = stack.shape
    bands, G = [], stack.astype(np.float32, copy=True)
    for _ in range(levels):
        Gb = blur_stack(G)
        h2, w2 = max(8, H // 2), max(8, W // 2)
        Gn = np.empty((T, h2, w2), np.float32)
        for t in range(T):
            Gn[t] = cv2.resize(Gb[t], (w2, h2),
                               interpolation=cv2.INTER_AREA)
        band = np.empty_like(Gb)
        for t in range(T):
            up = cv2.resize(Gn[t], (W, H), interpolation=cv2.INTER_LINEAR)
            band[t] = Gb[t] - up
        bands.append(band)
        G, H, W = Gn, h2, w2
    return bands, G


def synthesize(bands, residual):
    cur = residual
    for band in reversed(bands):
        T, H, W = band.shape
        out = np.empty_like(band)
        for t in range(T):
            up = cv2.resize(cur[t], (W, H), interpolation=cv2.INTER_LINEAR)
            out[t] = band[t] + up
        cur = out
    return cur


# ============================== 时域带通 ==============================
def temporal_bandpass(x, fps, lo, hi, mode):
    """x: (T, ...) 去均值并只保留 [lo,hi] Hz"""
    T = x.shape[0]
    if mode == "fft":
        F = np.fft.rfft(x, axis=0)
        f = np.fft.rfftfreq(T, 1.0 / fps)
        F[(f < lo) | (f > hi)] = 0.0
        return np.fft.irfft(F, n=T, axis=0).astype(np.float32)
    # 因果 RBJ 双二阶带通(贴近 FPGA IIR, 0dB 峰值增益)
    f0 = math.sqrt(lo * hi)
    Q = max(0.5, f0 / max(1e-6, hi - lo))
    w0 = 2 * math.pi * f0 / fps
    alpha = math.sin(w0) / (2 * Q)
    b0, b1, b2 = alpha, 0.0, -alpha
    a0, a1, a2 = 1 + alpha, -2 * math.cos(w0), 1 - alpha
    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
    y = np.empty_like(x)
    x1 = np.zeros(x.shape[1:], np.float32)
    x2 = np.zeros(x.shape[1:], np.float32)
    y1 = np.zeros(x.shape[1:], np.float32)
    y2 = np.zeros(x.shape[1:], np.float32)
    for t in range(T):
        xt = x[t]
        yt = b0 * xt + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, xt
        y2, y1 = y1, yt
        y[t] = yt
    return y


# ============================== 定点辅助 ==============================
def qstep(x, nbits):
    m = float(np.max(np.abs(x))) if x.size else 0.0
    return max(m, 1e-9) / (2 ** (nbits - 1) - 1)


def quant(x, nbits, step):
    hi = 2 ** (nbits - 1) - 1
    return np.clip(np.round(x / step), -hi - 1, hi).astype(np.float32) * step


# ============================== 相位放大核心 ==============================
def process_band(band, fps, lo, hi, alpha, mode, fx=0, tile_rows=48,
                 stats=None):
    """就地相位放大一个带 (T,H,W); Riesz 单演信号 + 时域带通 + 相位旋转"""
    T, H, W = band.shape
    halo, rows = 4, max(8, tile_rows)
    out = np.empty_like(band)

    # 定点定标预扫: 求 Rx/Ry 峰值(报告整数位宽用 + 量化步长)
    steps = {}
    if fx:
        mx = {"b": float(np.max(np.abs(band))), "rx": 0.0, "ry": 0.0}
        for y0 in range(0, H, rows):
            y1 = min(H, y0 + rows)
            a0, a1 = max(0, y0 - halo), min(H, y1 + halo)
            sl = band[:, a0:a1]
            for t in range(T):
                rx = sep2d(sl[t], K_ODD, K_EVEN)
                ry = sep2d(sl[t], K_EVEN, K_ODD)
                mx["rx"] = max(mx["rx"], float(np.max(np.abs(rx))))
                mx["ry"] = max(mx["ry"], float(np.max(np.abs(ry))))
        if stats is not None:
            stats.setdefault("peak", {}).update(mx)
        steps = {k: max(v, 1e-9) / (2 ** (fx - 1) - 1) for k, v in mx.items()}

    fx_err = 0.0
    for ti, y0 in enumerate(range(0, H, rows)):
        y1 = min(H, y0 + rows)
        a0, a1 = max(0, y0 - halo), min(H, y1 + halo)
        sl = band[:, a0:a1]
        Rx = np.empty_like(sl)
        Ry = np.empty_like(sl)
        for t in range(T):
            Rx[t] = sep2d(sl[t], K_ODD, K_EVEN)   # Riesz-x
            Ry[t] = sep2d(sl[t], K_EVEN, K_ODD)   # Riesz-y
        b = sl.copy()
        if fx:
            b = quant(b, fx, steps["b"])
            Rx = quant(Rx, fx, steps["rx"])
            Ry = quant(Ry, fx, steps["ry"])
        R = np.sqrt(Rx * Rx + Ry * Ry) + 1e-6
        phi = np.arctan2(b, R)                    # 单演局部相位
        phi = np.unwrap(phi, axis=0)              # 时域展开
        dphi = phi - phi[:1]                      # 相对首帧的相位偏移
        dphi = temporal_bandpass(dphi, fps, lo, hi, mode)
        delta = alpha * dphi                      # 放大
        if fx:
            delta = quant(delta, fx, qstep(delta, fx))
        c, s = np.cos(delta), np.sin(delta)
        bnew = b * c + R * s                      # (R,B) 平面相位旋转后取 B
        if fx:
            bnew = quant(bnew, fx, steps["b"])
        keep = slice(y0 - a0, y1 - a0)
        if fx and ti == 0:                        # 与浮点对照记录误差
            ref = (b * np.cos(alpha * dphi) +
                   np.sqrt(Rx * Rx + Ry * Ry + 1e-6) * np.sin(alpha * dphi))
            fx_err = float(np.max(np.abs(bnew - ref)))
            if stats is not None:
                stats["fx_err_tile0_max"] = fx_err
        out[:, y0:y1] = bnew[:, keep]
    band[...] = out


# ============================== 亚像素位移测量 ==============================
def track_motion(stack, fps, roi=None):
    """相位相关 + 抛物线亚像素: 相对首帧的 (dx,dy), 单位=像素"""
    T, H, W = stack.shape
    x0, y0, w, h = (0, 0, W, H)
    if roi:
        x0, y0, w, h = roi
    win = np.outer(np.hanning(h), np.hanning(w)).astype(np.float32)
    ref = stack[0, y0:y0 + h, x0:x0 + w].astype(np.float32) * win
    Fref = np.fft.fft2(ref)
    dx = np.zeros(T)
    dy = np.zeros(T)
    for t in range(T):
        cur = stack[t, y0:y0 + h, x0:x0 + w].astype(np.float32) * win
        Fcur = np.fft.fft2(cur)
        Cs = Fref * np.conj(Fcur)
        Cs /= np.abs(Cs) + 1e-9
        cc = np.fft.ifft2(Cs).real
        iy, ix = np.unravel_index(np.argmax(cc), cc.shape)

        def sub(cc, iy, ix, axis):
            v = lambda k: cc[iy, k % cc.shape[1]] if axis == 1 \
                else cc[k % cc.shape[0], ix]
            p, c0, n = v(ix - 1) if axis == 1 else v(iy - 1), \
                       v(ix) if axis == 1 else v(iy), \
                       v(ix + 1) if axis == 1 else v(iy + 1)
            den = p - 2 * c0 + n
            return 0.5 * (p - n) / den if abs(den) > 1e-12 else 0.0

        sx = ix + sub(cc, iy, ix, 1)
        sy = iy + sub(cc, iy, ix, 0)
        dx[t] = sx - w if sx > w / 2 else sx
        dy[t] = sy - h if sy > h / 2 else sy
    tt = np.arange(T, dtype=np.float64)
    for d in (dx, dy):
        d -= np.polyval(np.polyfit(tt, d, 1), tt)   # 去线性漂移
    return dx.astype(np.float32), dy.astype(np.float32)


def bandpass1d(d, fps, lo, hi):
    F = np.fft.rfft(d)
    f = np.fft.rfftfreq(len(d), 1.0 / fps)
    F[(f < lo) | (f > hi)] = 0.0
    return np.fft.irfft(F, n=len(d))


def band_metrics(d, fps, lo, hi):
    """带内 RMS / 峰峰 / 主频与幅值"""
    db = bandpass1d(d, fps, lo, hi)
    rms = float(np.sqrt(np.mean(db ** 2)))
    p2p = float(db.max() - db.min()) if db.size else 0.0
    win = np.hanning(len(d))
    F = np.abs(np.fft.rfft(d * win)) * 2.0 / max(1e-9, win.sum())
    f = np.fft.rfftfreq(len(d), 1.0 / fps)
    band = (f >= 0.5) & (f <= fps / 2)
    k = np.argmax(F * band)
    return dict(rms=rms, p2p=p2p, fdom=float(f[k]), amp=float(F[k]),
                series=db)


# ============================== 数据装载 ==============================
def load_input(path, fps_hint, start, maxframes, scale=1.0):
    ext = os.path.splitext(path)[1].lower()
    frames, fps = [], fps_hint

    def prep(fr):
        fr = fr.astype(np.float32)
        if abs(scale - 1.0) >= 1e-6:
            h, w = fr.shape
            fr = cv2.resize(fr, (max(8, int(w * scale)),
                                 max(8, int(h * scale))),
                            interpolation=cv2.INTER_AREA)
        return fr

    if os.path.isdir(path):
        files = sorted(glob.glob(os.path.join(path, "*.bin")) +
                       glob.glob(os.path.join(path, "*.raw")))
        for fp in files[start:start + maxframes]:
            a = np.fromfile(fp, dtype="<u2")
            if a.size == 800 * 1280:
                frames.append(prep(a.reshape(800, 1280) & 0x3ff))
            else:
                print(f"  跳过尺寸不符的 {os.path.basename(fp)} ({a.size})")
        fps = fps_hint or 30.0
    elif ext in (".bin", ".raw"):
        # 单文件帧序列(rec120 的 .raw 直接可用): memmap 逐帧读+边读边缩放
        a = np.memmap(path, dtype="<u2", mode="r")
        n = a.size // (800 * 1280)
        for i in range(start, min(n, start + maxframes)):
            fr = (a[i * 800 * 1280:(i + 1) * 800 * 1280]
                  .reshape(800, 1280) & 0x3ff)
            frames.append(prep(fr))
        fps = fps_hint or 30.0
    elif ext == ".npy":
        a = np.load(path)
        frames = [prep(a[i])
                  for i in range(start, min(len(a), start + maxframes))]
        fps = fps_hint or 30.0
    else:
        cap = cv2.VideoCapture(path)
        fps = fps_hint or cap.get(cv2.CAP_PROP_FPS) or 30.0
        cap.set(cv2.CAP_PROP_POS_FRAMES, start)
        while len(frames) < maxframes:
            ok, fr = cap.read()
            if not ok:
                break
            if fr.ndim == 3:
                fr = cv2.cvtColor(fr, cv2.COLOR_BGR2GRAY)
            frames.append(fr.astype(np.float32))
        cap.release()
    if len(frames) < 8:
        sys.exit(f"帧数不足: 仅读到 {len(frames)} 帧")
    return np.stack(frames), float(fps)


def scale_stack(stack, scale):
    if abs(scale - 1.0) < 1e-6:
        return stack
    T, H, W = stack.shape
    w2, h2 = max(8, int(W * scale)), max(8, int(H * scale))
    out = np.empty((T, h2, w2), np.float32)
    for t in range(T):
        out[t] = cv2.resize(stack[t], (w2, h2), interpolation=cv2.INTER_AREA)
    return out


def write_video(path, orig, mag, fps):
    T, H, W = orig.shape
    lo = np.percentile(orig, 0.5)
    hi = max(lo + 1.0, np.percentile(orig, 99.5))
    vw = cv2.VideoWriter(path, cv2.VideoWriter_fourcc(*"mp4v"),
                         fps, (W * 2 + 4, H), isColor=False)
    for t in range(T):
        o = np.clip((orig[t] - lo) / (hi - lo) * 255, 0, 255).astype(np.uint8)
        m = np.clip((mag[t] - lo) / (hi - lo) * 255, 0, 255).astype(np.uint8)
        frame = np.full((H, W * 2 + 4), 128, np.uint8)
        frame[:, :W] = o
        frame[:, W + 4:] = m
        vw.write(frame)
    vw.release()


def analyze(name, stack, fps, lo, hi, roi=None, kpx=1.0):
    dx, dy = track_motion(stack, fps, roi)
    if kpx != 1.0:                      # 换算回原始像素单位
        dx, dy = dx * float(kpx), dy * float(kpx)
    mx, my = band_metrics(dx, fps, lo, hi), band_metrics(dy, fps, lo, hi)
    return dict(name=name, fps=fps, T=stack.shape[0], dx=dx, dy=dy, mx=mx,
                my=my, shape=stack.shape[1:])


# ============================== 合成自检 ==============================
def fourier_shift(img, dx, dy):
    H, W = img.shape
    fy = np.fft.fftfreq(H)[:, None]
    fx = np.fft.fftfreq(W)[None, :]
    F = np.fft.fft2(img) * np.exp(-2j * np.pi * (fy * dy + fx * dx))
    return np.fft.ifft2(F).real.astype(np.float32)


def selftest(outdir, fx):
    print("== 自检: 合成 10bit 纹理 + 0.4px@8Hz / 0.25px@5Hz 亚像素振动 + 噪声σ2 ==")
    rng = np.random.default_rng(7)
    T, H, W, fps = 180, 180, 240, 30.0
    base = rng.normal(size=(H, W)).astype(np.float32)
    for _ in range(3):
        base = sep2d(base, K_EVEN, K_EVEN)
    base = (base - base.min()) / (base.max() - base.min()) * 900 + 60
    t = np.arange(T) / fps
    dx_gt = 0.40 * np.sin(2 * np.pi * 8.0 * t)
    dy_gt = 0.25 * np.sin(2 * np.pi * 5.0 * t)
    stack = np.empty((T, H, W), np.float32)
    for i in range(T):
        stack[i] = fourier_shift(base, dx_gt[i], dy_gt[i]) \
            + rng.normal(0, 2.0, (H, W)).astype(np.float32)
    idle = np.stack([base + rng.normal(0, 2.0, (H, W)).astype(np.float32)
                     for _ in range(T)])
    lo, hi, alpha = 3.0, 12.0, 20.0

    r_sig = analyze("selftest", stack, fps, lo, hi)
    r_base = analyze("selftest-idle", idle, fps, lo, hi)

    bands, resid = build_bands(stack, levels=3)
    st = {}
    for b in bands:
        process_band(b, fps, lo, hi, alpha, "fft", fx=fx, stats=st)
    mag = synthesize(bands, resid)
    os.makedirs(outdir, exist_ok=True)
    write_video(os.path.join(outdir, "mag_sbs.mp4"), stack, mag, fps)

    ok = True

    def check(label, cond, detail):
        nonlocal ok
        ok = ok and cond
        print(f"  [{'PASS' if cond else 'FAIL'}] {label}: {detail}")

    check("dx 主频≈8Hz", abs(r_sig["mx"]["fdom"] - 8.0) < 0.5,
          f"{r_sig['mx']['fdom']:.2f}Hz")
    check("dy 主频≈5Hz", abs(r_sig["my"]["fdom"] - 5.0) < 0.5,
          f"{r_sig['my']['fdom']:.2f}Hz")
    check("dx 位移 RMS≈0.28px", 0.15 < r_sig["mx"]["rms"] < 0.45,
          f"{r_sig['mx']['rms']:.3f}px (理论 0.40/√2=0.283)")
    check("dy 位移 RMS≈0.18px", 0.08 < r_sig["my"]["rms"] < 0.32,
          f"{r_sig['my']['rms']:.3f}px (理论 0.25/√2=0.177)")
    margin = 20 * math.log10(max(r_sig["mx"]["rms"], 1e-9) /
                             max(r_base["mx"]["rms"], 1e-9))
    check("信噪余量>10dB", margin > 10.0,
          f"{margin:.1f}dB (静止噪声底线 {r_base['mx']['rms']:.4f}px)")
    if fx:
        e = st.get("fx_err_tile0_max", 0.0)
        check(f"{fx}bit 定点误差<0.5DN", e < 0.5, f"max|Δ|={e:.4f}DN")
    print(f"  输出: {outdir}/mag_sbs.mp4")
    print("== 自检" + ("全部通过 ✓" if ok else "存在失败 ✗ (请反馈输出)") + " ==")
    return 0 if ok else 1


# ============================== 主流程 ==============================
def main():
    ap = argparse.ArgumentParser(description="相位式运动放大离线验证工具")
    ap.add_argument("--input", help="视频文件 / raw_dump 目录 / .npy 栈")
    ap.add_argument("--baseline", help="静止对照视频(测噪声底线)")
    ap.add_argument("--selftest", action="store_true", help="合成数据自检")
    ap.add_argument("--lo", type=float, default=3.0, help="频带下限 Hz")
    ap.add_argument("--hi", type=float, default=15.0, help="频带上限 Hz")
    ap.add_argument("--alpha", type=float, default=12.0, help="相位放大倍数")
    ap.add_argument("--levels", type=int, default=3, help="金字塔层数")
    ap.add_argument("--scale", type=float, default=0.5,
                    help="工作分辨率缩放(0.5=半分辨率, 降采样换SNR)")
    ap.add_argument("--fps", type=float, default=0.0, help="覆盖帧率")
    ap.add_argument("--start", type=int, default=0, help="起始帧")
    ap.add_argument("--maxframes", type=int, default=240, help="最大帧数")
    ap.add_argument("--filt", choices=("fft", "iir"), default="fft",
                    help="时域带通: fft=理想零相位(默认) iir=因果(贴近FPGA)")
    ap.add_argument("--fx", type=int, default=0,
                    help="定点位宽评估, 如 --fx 12 (0=关闭)")
    ap.add_argument("--roi", default="",
                    help="位移测量ROI: x,y,w,h (工作分辨率坐标)")
    ap.add_argument("--outdir", default="phasemag_out")
    ap.add_argument("--tile-rows", type=int, default=48)
    ap.add_argument("--no-video", action="store_true")
    a = ap.parse_args()

    if a.selftest:
        sys.exit(selftest(a.outdir, a.fx))
    if not a.input:
        ap.error("需要 --input 或 --selftest")

    roi = tuple(int(v) for v in a.roi.split(",")) if a.roi else None
    os.makedirs(a.outdir, exist_ok=True)
    report = []

    def log(s):
        print(s)
        report.append(s)

    stack, fps = load_input(a.input, a.fps, a.start, a.maxframes, a.scale)
    T, H0, W0 = stack.shape
    log(f"输入: {a.input}  {T}帧 原始约{W0/a.scale:.0f}x{H0/a.scale:.0f}"
        f" -> 工作 {W0}x{H0} @{fps:.1f}fps  (位移报告=原始像素)")
    log(f"参数: band={a.lo}~{a.hi}Hz  alpha={a.alpha}  levels={a.levels}  "
        f"scale={a.scale}  filt={a.filt}  fx={a.fx or 'off'}")

    sig = analyze("signal", stack, fps, a.lo, a.hi, roi, kpx=1.0 / a.scale)
    log(f"位移(原始像素): dx 主频 {sig['mx']['fdom']:.2f}Hz "
        f"带内RMS {sig['mx']['rms']:.3f}px 峰峰 {sig['mx']['p2p']:.3f}px | "
        f"dy {sig['my']['fdom']:.2f}Hz RMS {sig['my']['rms']:.3f}px")

    if a.baseline:
        bstack, bfps = load_input(a.baseline, a.fps or fps, 0,
                                  a.maxframes, a.scale)
        base = analyze("idle", bstack, bfps, a.lo, a.hi, roi,
                       kpx=1.0 / a.scale)
        for ax in ("mx", "my"):
            m = 20 * math.log10(max(sig[ax]["rms"], 1e-9) /
                                max(base[ax]["rms"], 1e-9))
            log(f"噪声底线[{ax[1]}]: {base[ax]['rms']:.4f}px  余量 {m:.1f}dB")

    # 运动放大(装载时已按 scale 缩放)
    work = stack
    log(f"工作分辨率: {work.shape[2]}x{work.shape[1]}  "
        f"峰值内存约 {work.size*4*(a.levels+2)/2**20:.0f}MB")
    bands, resid = build_bands(work, a.levels)
    st = {}
    for b in bands:
        process_band(b, fps, a.lo, a.hi, a.alpha, a.filt, fx=a.fx,
                     tile_rows=a.tile_rows, stats=st)
    mag = synthesize(bands, resid)
    if a.fx and st:
        pk = st.get("peak", {})
        log(f"定点定标建议({a.fx}bit): 峰值 |B|={pk.get('b', 0):.1f} "
            f"|Rx|={pk.get('rx', 0):.1f} |Ry|={pk.get('ry', 0):.1f} DN "
            f"-> 按此设整数位; tile0 浮点对照 max|Δ|="
            f"{st.get('fx_err_tile0_max', 0):.4f}DN")

    if not a.no_video:
        vp = os.path.join(a.outdir, "mag_sbs.mp4")
        write_video(vp, work, mag, fps)
        log(f"视频: {vp} (左原|右放大 x{a.alpha})")

    # 位移曲线 CSV + 图
    csv = os.path.join(a.outdir, "motion.csv")
    t = np.arange(sig["T"]) / fps
    np.savetxt(csv, np.stack([t, sig["dx"], sig["dy"],
                             sig["mx"]["series"], sig["my"]["series"]], 1),
               delimiter=",", header="t,dx,dy,dx_band,dy_band", comments="")
    log(f"位移序列: {csv}")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(2, 2, figsize=(11, 7))
        ax[0][0].plot(t, sig["dx"], lw=0.8)
        ax[0][0].set_title("dx raw (px)")
        ax[0][1].plot(t, sig["mx"]["series"], lw=0.8, color="C3")
        ax[0][1].set_title(f"dx {a.lo}-{a.hi}Hz (px)")
        f = np.fft.rfftfreq(sig["T"], 1 / fps)
        for arr, col in ((sig["dx"], "C0"), (sig["dy"], "C2")):
            w = np.hanning(len(arr))
            F = np.abs(np.fft.rfft(arr * w)) * 2 / w.sum()
            ax[1][0].plot(f, F, lw=0.8, color=col, label="dx" if col == "C0"
                          else "dy")
        ax[1][0].axvspan(a.lo, a.hi, color="k", alpha=0.1)
        ax[1][0].set_xlim(0, fps / 2)
        ax[1][0].set_title("displacement spectrum (px)")
        ax[1][0].legend()
        ax[1][1].plot(t, sig["my"]["series"], lw=0.8, color="C2")
        ax[1][1].set_title(f"dy {a.lo}-{a.hi}Hz (px)")
        fig.tight_layout()
        fig.savefig(os.path.join(a.outdir, "report.png"), dpi=110)
        log(f"图: {os.path.join(a.outdir, 'report.png')}")
    except ImportError:
        log("(matplotlib 未装, 跳过出图; CSV 已有全部数据)")

    with open(os.path.join(a.outdir, "report.txt"), "w") as f:
        f.write("\n".join(report) + "\n")


if __name__ == "__main__":
    main()
