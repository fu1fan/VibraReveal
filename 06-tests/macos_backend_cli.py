#!/usr/bin/env python3
"""Dependency-free binary-stream integration checks. Pass the built CLI path."""
import json
import math
import pathlib
import tempfile
import struct
import subprocess
import sys
import unittest

BINARY = sys.argv.pop(1)


class StreamTests(unittest.TestCase):
    def run_backend(self, data, *extra):
        return subprocess.run(
            [BINARY, "stream", "--width", "17", "--height", "13", "--fps", "59.94",
             "--alpha", "0", *extra], input=data, capture_output=True, timeout=30)

    def test_stream_framing_metadata_pixels_and_latest_frame(self):
        source = bytes((x * 17 + y * 13) % 256 for y in range(13) for x in range(17))
        process = self.run_backend(source * 100)
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        output = process.stdout
        offset = 0
        frames = []
        while offset < len(output):
            length, = struct.unpack_from("<I", output, offset)
            offset += 4
            header = json.loads(output[offset:offset + length])
            offset += length
            count = header["width"] * header["height"]
            pixels = struct.unpack_from(f"<{count}f", output, offset)
            offset += count * 4
            self.assertEqual((header["width"], header["height"]), (17, 13))
            self.assertEqual(header["version"], 1)
            self.assertEqual(header["pixelFormat"], "float32le")
            self.assertAlmostEqual(header["timestamp"], header["sequence"] / 59.94)
            if not frames or header["sequence"] != frames[-1]["sequence"] + 1:
                self.assertTrue(header["historyReset"])
            for actual, original in zip(pixels, source):
                self.assertAlmostEqual(actual, original / 255, places=5)
            frames.append(header)
        self.assertEqual(offset, len(output))
        self.assertGreater(len(frames), 0)
        self.assertEqual(frames[-1]["sequence"], 99)
        self.assertIn(b"failed=0", process.stderr)

    def test_stream_telemetry_accounts_for_every_frame_and_preserves_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            report = pathlib.Path(directory) / "frames.jsonl"
            process = self.run_backend(bytes(17 * 13 * 100), "--telemetry", str(report),
                                       "--discard-output", "true", "--adaptive", "false")
            self.assertEqual(process.returncode, 0, process.stderr.decode())
            self.assertEqual(process.stdout, b"")
            original = report.read_bytes()
            events = [json.loads(line) for line in original.splitlines()]
            received = [e for e in events if e["event"] == "received"]
            completed = [e for e in events if e["event"] == "processed"]
            dropped = [e for e in events if e["event"] == "dropped"]
            summary = events[-1]
            self.assertEqual(summary["event"], "summary")
            self.assertEqual(summary["submitted"], 100)
            self.assertEqual(summary["completed"], len(completed))
            self.assertEqual(summary["dropped"], len(dropped))
            self.assertEqual(summary["failed"], 0)
            self.assertEqual(len(completed) + len(dropped), 100)
            self.assertEqual({e["sequence"] for e in received}, set(range(100)))
            self.assertEqual({e["sequence"] for e in completed + dropped}, set(range(100)))
            for frame in completed:
                self.assertGreaterEqual(frame["completed"], frame["received"])
                self.assertGreater(frame["processingSeconds"], 0)
                self.assertEqual((frame["width"], frame["height"]), (17, 13))
            again = self.run_backend(b"", "--telemetry", str(report))
            self.assertNotEqual(again.returncode, 0)
            self.assertEqual(report.read_bytes(), original)

    def test_offline_never_drops_or_downscales(self):
        width, height, count = 37, 29, 50
        source = bytes((x * 7) % 256 for x in range(width * height))
        process = subprocess.run(
            [BINARY, "offline", "--width", str(width), "--height", str(height),
             "--fps", "10000", "--low", "100", "--high", "500", "--alpha", "0",
             "--max-pixels", "256"], input=source * count, capture_output=True, timeout=30)
        self.assertEqual(process.returncode, 0, process.stderr.decode())
        offset = 0
        for sequence in range(count):
            length, = struct.unpack_from("<I", process.stdout, offset)
            offset += 4
            header = json.loads(process.stdout[offset:offset + length])
            offset += length
            self.assertEqual((header["width"], header["height"]), (width, height))
            self.assertEqual(header["sequence"], sequence)
            self.assertEqual(header["mode"], "offline")
            self.assertEqual(header["temporalFilter"], "zeroPhase")
            self.assertEqual(header["performance"]["status"], "offline")
            pixels = struct.unpack_from(f"<{width * height}f", process.stdout, offset)
            for actual, original in zip(pixels, source):
                self.assertAlmostEqual(actual, original / 255, places=5)
            offset += width * height * 4
        self.assertEqual(offset, len(process.stdout))
        self.assertIn(b"completed=50 dropped=0", process.stderr)

    def test_offline_spectrum_json_gain200_and_scratch_cleanup(self):
        width, height, count, fps = 21, 17, 256, 128
        samples = []
        for n in range(count):
            shift = 0.02 * math.sin(2 * math.pi * 8 * n / fps)
            for y in range(height):
                for x in range(width):
                    samples.append(round(65535 * (0.5 + 0.3 * math.sin((x+shift)*0.5) * math.cos(y*0.23))))
        with tempfile.TemporaryDirectory() as directory:
            report_path = pathlib.Path(directory) / "spectrum.json"
            scratch = pathlib.Path(directory) / "scratch"
            process = subprocess.run(
                [BINARY, "offline", "--width", str(width), "--height", str(height),
                 "--fps", str(fps), "--low", "5", "--high", "12", "--alpha", "200",
                 "--bits", "16", "--spectrum", str(report_path), "--probe", "7,5",
                 "--scratch-dir", str(scratch)],
                input=struct.pack(f"<{len(samples)}H", *samples), capture_output=True, timeout=30)
            self.assertEqual(process.returncode, 0, process.stderr.decode())
            report = json.loads(report_path.read_text())
            self.assertEqual(report["frameCount"], count)
            self.assertEqual(report["temporalFilter"], "zeroPhase")
            self.assertEqual(report["spectra"]["amplification"], 200)
            for key in ("input", "filtered"):
                spectrum = report["spectra"][key]
                self.assertEqual(spectrum["peakFrequencyHz"], 8)
                self.assertEqual(spectrum["amplitudeUnit"], "rad")
                self.assertEqual(spectrum["psdUnit"], "rad^2/Hz")
                self.assertEqual(len(spectrum["frequencyHz"]), count // 2 + 1)
            self.assertEqual(list(scratch.iterdir()), [])
            offset = 0
            for n in range(count):
                length, = struct.unpack_from("<I", process.stdout, offset)
                offset += 4
                header = json.loads(process.stdout[offset:offset+length])
                offset += length
                self.assertEqual(header["sequence"], n)
                pixels = struct.unpack_from(f"<{width*height}f", process.stdout, offset)
                self.assertTrue(all(math.isfinite(x) for x in pixels))
                offset += width * height * 4
            self.assertEqual(offset, len(process.stdout))

    def test_filter_and_gain_validation(self):
        self.assertNotEqual(self.run_backend(b"", "--filter", "zero-phase").returncode, 0)
        self.assertNotEqual(self.run_backend(b"", "--alpha", "201").returncode, 0)
        self.assertNotEqual(self.run_backend(b"", "--filter", "invalid").returncode, 0)
        causal = subprocess.run(
            [BINARY, "offline", "--width", "17", "--height", "13", "--fps", "120",
             "--filter", "causal", "--alpha", "200"], input=bytes(17*13*4), capture_output=True, timeout=30)
        self.assertEqual(causal.returncode, 0, causal.stderr.decode())
        length, = struct.unpack_from("<I", causal.stdout)
        self.assertEqual(json.loads(causal.stdout[4:4+length])["temporalFilter"], "causal")

    def test_partial_frame_is_an_error(self):
        process = self.run_backend(b"\0" * 220)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn(b"Truncated", process.stderr)

    def test_invalid_frequency_is_an_error(self):
        process = self.run_backend(b"", "--high", "30")
        self.assertNotEqual(process.returncode, 0)

    def test_invalid_roi_is_an_error(self):
        process = self.run_backend(b"", "--roi", "16,0,2,2")
        self.assertNotEqual(process.returncode, 0)


if __name__ == "__main__":
    unittest.main()
