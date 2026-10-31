import Foundation
import VibraRevealBackend
import VibraRevealTransport

let usage = """
VibraReveal Metal backend (no GUI)
  vibrareveal benchmark --width 1280 --height 800 --fps 120 --frames 120
  vibrareveal stream --width W --height H --fps FPS --low HZ --high HZ [options]
  vibrareveal offline --width W --height H --fps FPS --low HZ --high HZ [options]

stream reads unpacked grayscale frames from stdin as they arrive (no artificial pacing).
offline spools the clip to disk, applies zero-phase filtering, then emits every frame at full resolution.
The FPS in both modes is the source sampling rate, not the speed of processing.
stdout: per frame, UInt32 LE JSON length + UTF-8 JSON + width*height Float32 LE pixels.
Metadata includes source sequence/time, ROI, processing size, reset and performance.
Input timestamps are synthesized as sequence/fps; use the Swift API for capture timestamps.
A partial frame at EOF is an error. Output is the processed ROI, not a full-frame composite.

Options: --bits 8|10|12|16 (default 8; >8 uses right-aligned uint16 LE)
         --stride BYTES (default tightly packed) --roi x,y,width,height
         --alpha 10 (0...200) --levels 3 --max-pixels 1048576 --min-pixels 256
         --low 3 --high 12 --frames 120 (benchmark only) --adaptive true|false
         --mode realtime|offline (benchmark only; default realtime)
         --filter auto|causal|zero-phase (auto: realtime causal, offline zero-phase)
         --spectrum report.json --probe x,y --spectrum-level 0 (offline only)
         --scratch-dir PATH (offline scratch storage; default system temporary directory)
         --connect IPv4 --port 5910 (USB NCM stream; dimensions/rate/time from protocol)
         --archive capture.vri (verified transport archive; dimensions/rate/time from protocol)
"""

func stderr(_ message: String) { FileHandle.standardError.write(Data((message + "\n").utf8)) }
struct Header: Encodable {
    let temporalFilter: TemporalFilter
    let mode: ProcessingMode
    let version = 1
    let pixelFormat = "float32le"
    let width: Int, height: Int, sourceWidth: Int, sourceHeight: Int
    let sequence: UInt64, timestamp: Double, historyReset: Bool
    let roi: Region, performance: Performance
}
func writeOutput(_ frame: OutputFrame) throws {
    let header = Header(temporalFilter: frame.temporalFilter, mode: frame.mode, width: frame.width, height: frame.height, sourceWidth: frame.sourceWidth,
        sourceHeight: frame.sourceHeight, sequence: frame.sequence, timestamp: frame.timestamp,
        historyReset: frame.historyReset, roi: frame.roi, performance: frame.performance)
    let json = try JSONEncoder().encode(header)
    var length = UInt32(json.count).littleEndian
    try withUnsafeBytes(of: &length) { try FileHandle.standardOutput.write(contentsOf: Data($0)) }
    try FileHandle.standardOutput.write(contentsOf: json)
    try frame.pixels.withUnsafeBytes { try FileHandle.standardOutput.write(contentsOf: Data($0)) }
}
func run() throws {
    let arguments = Array(CommandLine.arguments.dropFirst())
    guard let mode = arguments.first, mode != "--help", mode != "help" else { print(usage); return }
    guard ["stream", "offline", "benchmark"].contains(mode) else { throw BackendError.invalid(usage) }
    var options: [String: String] = [:]
    var i = 1
    let allowed: Set<String> = ["width","height","fps","low","high","alpha","levels","max-pixels","min-pixels","frames","bits","stride","roi","adaptive","mode","filter","spectrum","probe","spectrum-level","scratch-dir","connect","port","archive"]
    while i < arguments.count {
        let key = String(arguments[i].dropFirst(2))
        guard arguments[i].hasPrefix("--"), allowed.contains(key), i+1 < arguments.count, options[key] == nil else {
            throw BackendError.invalid("Unknown, duplicate or incomplete option: \(arguments[i])")
        }
        options[key] = arguments[i+1]; i += 2
    }
    func integer(_ key: String, _ fallback: Int) throws -> Int {
        guard let text = options[key] else { return fallback }
        guard let value = Int(text) else { throw BackendError.invalid("Invalid integer: --\(key)") }
        return value
    }
    func number(_ key: String, _ fallback: Double) throws -> Double {
        guard let text = options[key] else { return fallback }
        guard let value = Double(text), value.isFinite else { throw BackendError.invalid("Invalid number: --\(key)") }
        return value
    }
    guard options["connect"] == nil || options["archive"] == nil else {
        throw BackendError.invalid("Choose --connect or --archive")
    }
    let networkInput = options["connect"] != nil || options["archive"] != nil
    guard options["port"] == nil || options["connect"] != nil else { throw BackendError.invalid("--port requires --connect") }
    if networkInput && (mode == "benchmark" || ["width", "height", "bits", "fps", "stride"].contains(where: { options[$0] != nil })) {
        throw BackendError.invalid("Transport input supplies dimensions, format and FPS; do not override them")
    }
    var transport: FrameReader?
    if let host = options["connect"] {
        let port = try integer("port", 5910)
        guard (1...65535).contains(port) else { throw BackendError.invalid("Invalid port") }
        transport = try FrameReader(host: host, port: UInt16(port))
        guard transport!.info.realtime == (mode == "stream") else { throw BackendError.invalid("Sender/processor mode mismatch") }
    } else if let path = options["archive"] {
        transport = try FrameReader(archiveURL: URL(fileURLWithPath: path))
    }
    let width = try transport?.info.width ?? integer("width", 1280)
    let height = try transport?.info.height ?? integer("height", 800)
    let bits = try transport?.info.bits ?? integer("bits", 8)
    let fps = try transport?.info.fps ?? number("fps", 120)
    guard width > 0, height > 0, width <= 65535, height <= 65535, [8,10,12,16].contains(bits) else {
        throw BackendError.invalid("Invalid source dimensions or bit depth")
    }
    let stride = try transport?.info.stride ?? integer("stride", width * (bits == 8 ? 1 : 2))
    var roi: Region?
    if let text = options["roi"] {
        let fields = text.split(separator: ",", omittingEmptySubsequences: false)
        let v = fields.compactMap { Int($0) }
        guard v.count == 4 else { throw BackendError.invalid("ROI must be x,y,width,height") }
        roi = Region(x: v[0], y: v[1], width: v[2], height: v[3])
    }
    let adaptive = options["adaptive"] ?? "true"
    guard ["true","false"].contains(adaptive) else { throw BackendError.invalid("--adaptive must be true or false") }
    guard options["mode"] == nil || mode == "benchmark" else {
        throw BackendError.invalid("--mode is only used by benchmark; choose the stream or offline command")
    }
    guard let processingMode = ProcessingMode(rawValue: mode == "offline" ? "offline" : (options["mode"] ?? "realtime")) else {
        throw BackendError.invalid("--mode must be realtime or offline")
    }
    let filterNames: [String: TemporalFilter] = ["auto": .automatic, "causal": .causal, "zero-phase": .zeroPhase]
    guard let temporalFilter = filterNames[options["filter"] ?? "auto"] else {
        throw BackendError.invalid("--filter must be auto, causal or zero-phase")
    }
    if options["spectrum"] != nil && mode != "offline" {
        throw BackendError.invalid("--spectrum is available on the offline command; use SpectrumAnalyzer for custom streams")
    }
    if (options["probe"] != nil || options["spectrum-level"] != nil) && options["spectrum"] == nil {
        throw BackendError.invalid("--probe and --spectrum-level require --spectrum")
    }
    let config = try Configuration(fps: fps, lowHz: number("low", 3), highHz: number("high", 12),
        amplification: Float(number("alpha", 10)), levels: integer("levels", 3), roi: roi,
        maxWorkingPixels: integer("max-pixels", 1_048_576), minWorkingPixels: integer("min-pixels", 256),
        adaptive: adaptive == "true", mode: processingMode, temporalFilter: temporalFilter)
    try config.validate()
    let inputLimit = processingMode == .offline ? Int(UInt32.max) : config.maxInputBytes
    guard stride >= width * (bits == 8 ? 1 : 2), stride <= inputLimit / height else {
        throw BackendError.invalid("Invalid stride or input exceeds memory budget")
    }
    if let roi {
        guard roi.x >= 0, roi.y >= 0, roi.width > 0, roi.height > 0,
              roi.x <= width, roi.y <= height, roi.width <= width-roi.x, roi.height <= height-roi.y else {
            throw BackendError.invalid("ROI outside input")
        }
    }
    let byteCount = stride * height
    let format: PixelFormat = bits == 8 ? .gray8 : .gray16LE(validBits: bits)
    func readFrame(sequence: UInt64) throws -> InputFrame? {
        if let transport { return try transport.readNext() }
        var data = Data()
        while data.count < byteCount {
            guard let chunk = try FileHandle.standardInput.read(upToCount: byteCount-data.count), !chunk.isEmpty else { break }
            data.append(chunk)
        }
        if data.isEmpty { return nil }
        guard data.count == byteCount else { throw BackendError.invalid("Truncated input frame at EOF") }
        return InputFrame(data: data, width: width, height: height, bytesPerRow: stride,
            format: format, sequence: sequence, timestamp: Double(sequence)/fps)
    }
    if mode == "benchmark" {
        let count = try integer("frames", 120)
        guard count >= 8, count <= 100000 else { throw BackendError.invalid("Benchmark requires 8...100000 frames") }
        func syntheticFrame(_ sequence: Int) -> InputFrame {
            var data = Data(count: byteCount)
            data.withUnsafeMutableBytes { (bytes: UnsafeMutableRawBufferPointer) in
                let movement = 0.15 * sin(2 * Double.pi * sqrt(config.lowHz/fps) * sqrt(config.highHz/fps) * Double(sequence))
                for y in 0..<height { for x in 0..<width {
                    let v = UInt16((0.5 + 0.35 * sin((Double(x)+movement)*0.25) * cos(Double(y)*0.12)) * Double((1<<bits)-1))
                    let offset = y*stride + x*(bits == 8 ? 1 : 2)
                    bytes[offset] = UInt8(truncatingIfNeeded: v)
                    if bits != 8 { bytes[offset+1] = UInt8(v >> 8) }
                } }
            }
            return InputFrame(data: data, width: width, height: height,
                bytesPerRow: stride, format: format, sequence: UInt64(sequence), timestamp: Double(sequence)/fps)
        }
        if processingMode == .offline {
            let processor = try OfflineMagnifier(configuration: config)
            stderr("Metal device: \(processor.deviceName)")
            var index = 0
            let report = try processor.process(
                scratchDirectory: options["scratch-dir"].map { URL(fileURLWithPath: $0, isDirectory: true) } ?? FileManager.default.temporaryDirectory,
                readNext: { guard index < count else { return nil }; defer { index += 1 }; return syntheticFrame(index) },
                emit: { _ in })
            let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            print(String(data: try encoder.encode(report), encoding: .utf8)!)
        } else {
            let processor = try MetalMagnifier(configuration: config)
            stderr("Metal device: \(processor.deviceName)")
            var times: [Double] = [], last: OutputFrame?
            for index in 0..<count {
                let output = try processor.process(syntheticFrame(index))
                times.append(output.performance.processingSeconds); last = output
            }
            let sorted = times.sorted(), final = last!
            let report: [String: Any] = ["device": processor.deviceName, "mode": processingMode.rawValue,
                "temporalFilter": config.resolvedFilter.rawValue, "frames": count,
                "source": [width,height], "working": [final.width,final.height], "sourceFPS": fps,
                "medianMs": sorted[sorted.count/2]*1000,
                "p95Ms": sorted[min(sorted.count-1,Int(Double(sorted.count)*0.95))]*1000,
                "finalPerformance": try JSONSerialization.jsonObject(with: JSONEncoder().encode(final.performance)),
                "note": "Compute + upload/readback only; excludes synthetic source, transport and downstream sink."]
            print(String(data: try JSONSerialization.data(withJSONObject: report, options: [.prettyPrinted,.sortedKeys]), encoding: .utf8)!)
        }
    } else if mode == "offline" {
        let processor = try OfflineMagnifier(configuration: config)
        stderr("Metal device: \(processor.deviceName); offline \(config.resolvedFilter.rawValue) filtering")
        var probe: SpectrumProbe?
        if options["spectrum"] != nil {
            let level = try integer("spectrum-level", 0)
            guard level >= 0, level < config.levels else { throw BackendError.invalid("Invalid spectrum level") }
            var w = roi?.width ?? width, h = roi?.height ?? height
            for _ in 0..<level { w = max(1,w/2); h = max(1,h/2) }
            var x = w/2, y = h/2
            if let text = options["probe"] {
                let parts = text.split(separator: ",", omittingEmptySubsequences: false).compactMap { Int($0) }
                guard parts.count == 2 else { throw BackendError.invalid("--probe must be x,y") }
                x = parts[0]; y = parts[1]
            }
            probe = SpectrumProbe(level: level, x: x, y: y)
        }
        var sequence: UInt64 = 0
        let report = try processor.process(
            scratchDirectory: options["scratch-dir"].map { URL(fileURLWithPath: $0, isDirectory: true) } ?? FileManager.default.temporaryDirectory,
            spectrumProbe: probe,
            readNext: { defer { sequence &+= 1 }; return try readFrame(sequence: sequence) },
            emit: writeOutput)
        if let path = options["spectrum"] {
            let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            try encoder.encode(report).write(to: URL(fileURLWithPath: path), options: .atomic)
        }
        stderr("completed=\(report.frameCount) dropped=0 filter=\(report.temporalFilter.rawValue) padding=\(report.paddingFramesPerEnd) totalSeconds=\(report.processingSeconds)")
    } else {
        let errorLock = NSLock()
        var outputError: Error?
        let backend = try RealtimeMagnifier(configuration: config) { result in
            do { try writeOutput(result.get()) }
            catch { errorLock.lock(); outputError = error; errorLock.unlock() }
        }
        stderr("Metal device: \(backend.deviceName)")
        defer { backend.close(); backend.waitUntilIdle() }
        var sequence: UInt64 = 0
        while true {
            errorLock.lock(); let failure = outputError; errorLock.unlock()
            if let failure { throw failure }
            guard let frame = try readFrame(sequence: sequence) else { break }
            let admission = backend.submit(frame)
            if case .rejected(let reason) = admission { throw BackendError.invalid(reason) }
            sequence &+= 1
        }
        backend.close(); backend.waitUntilIdle()
        errorLock.lock(); let failure = outputError; errorLock.unlock()
        if let failure { throw failure }
        let stats = backend.statistics
        stderr("submitted=\(stats.submitted) completed=\(stats.completed) dropped=\(stats.dropped) failed=\(stats.failed)")
    }
}
do { try run() } catch { stderr("error: \(error)"); exit(1) }
