import Foundation

/// Fixed-record scratch storage: O(one frame) RAM, O(clip duration) disk space.
final class DiskRecords {
    private let handle: FileHandle
    let recordBytes: Int
    init(url: URL, recordBytes: Int) throws {
        self.recordBytes = recordBytes
        guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
            throw BackendError.resource("Cannot create scratch file: \(url.path)")
        }
        handle = try FileHandle(forUpdating: url)
    }
    deinit { try? handle.close() }
    private func seek(_ index: Int) throws {
        let (offset, overflow) = index.multipliedReportingOverflow(by: recordBytes)
        guard index >= 0, !overflow else { throw BackendError.resource("Scratch offset overflow") }
        try handle.seek(toOffset: UInt64(offset))
    }
    func write(_ data: Data, at index: Int) throws {
        guard data.count == recordBytes else { throw BackendError.invalid("Scratch record size mismatch") }
        try seek(index); try handle.write(contentsOf: data)
    }
    func read(_ index: Int) throws -> Data {
        try seek(index)
        var result = Data()
        while result.count < recordBytes {
            guard let chunk = try handle.read(upToCount: recordBytes - result.count), !chunk.isEmpty else {
                throw BackendError.resource("Truncated offline scratch file")
            }
            result.append(chunk)
        }
        return result
    }
    func writeFloats(_ values: [Float], at index: Int) throws {
        try values.withUnsafeBytes { try write(Data($0), at: index) }
    }
    func floats(_ index: Int) throws -> [Float] {
        let data = try read(index)
        var result = [Float](repeating: 0, count: recordBytes / 4)
        _ = result.withUnsafeMutableBytes { data.copyBytes(to: $0) }
        return result
    }
}

/// Double-precision recurrence, independently for every spatial phase coefficient.
struct PhaseIIR {
    private let c: [Double]
    private var x1: [Double], x2: [Double], y1: [Double], y2: [Double]
    init(coefficients: [Float], endpoint: [Float]) {
        c = coefficients.map(Double.init)
        x1 = endpoint.map(Double.init); x2 = x1
        y1 = Array(repeating: 0, count: endpoint.count); y2 = y1
    }
    mutating func apply(_ values: [Float]) -> [Float] {
        var result = [Float](repeating: 0, count: values.count)
        for i in values.indices {
            let x = Double(values[i])
            let y = c[0]*x + c[1]*x1[i] + c[2]*x2[i] - c[3]*y1[i] - c[4]*y2[i]
            result[i] = Float(y)
            x2[i] = x1[i]; x1[i] = x; y2[i] = y1[i]; y1[i] = y
        }
        return result
    }
}

public struct OfflineReport: Codable {
    public let frameCount: Int
    public let sampleRate: Double
    public let lowHz: Double, highHz: Double
    public let amplification: Float
    public let sourceWidth: Int, sourceHeight: Int
    public let roi: Region
    public let firstSequence: UInt64
    public let firstTimestamp: Double
    public let temporalFilter: TemporalFilter
    public let paddingFramesPerEnd: Int
    public let processingSeconds: Double
    public let spectra: PhaseSpectra?
}

/// Complete-clip processing: phase extraction -> temporal filtering -> reconstruction.
/// Spools raw/phase records to a private temporary directory; never drops or resizes.
/// A clip must have fixed geometry/format and contiguous, uniformly sampled timestamps.
public final class OfflineMagnifier {
    public let configuration: Configuration
    public let deviceName: String
    private let metal: MetalMagnifier
    public init(configuration: Configuration) throws {
        guard configuration.mode == .offline else { throw BackendError.invalid("OfflineMagnifier requires offline mode") }
        try configuration.validate()
        self.configuration = configuration
        metal = try MetalMagnifier(configuration: configuration)
        deviceName = metal.deviceName
    }

    /// readNext and emit run synchronously; no complete video is retained in RAM.
    /// The caller owns outputs already emitted if emit throws. Scratch is cleaned on all exits.
    public func process(scratchDirectory: URL = FileManager.default.temporaryDirectory,
                        spectrumProbe: SpectrumProbe? = nil,
                        readNext: () throws -> InputFrame?,
                        emit: (OutputFrame) throws -> Void) throws -> OfflineReport {
        let started = ProcessInfo.processInfo.systemUptime
        let directory = scratchDirectory.appendingPathComponent("vibrareveal-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory); metal.reset() }
        guard let first = try readNext() else { throw BackendError.invalid("Offline clip is empty") }
        let firstPhases = try metal.extractPhases(first)
        let roi = configuration.roi ?? Region(x: 0, y: 0, width: first.width, height: first.height)
        let sizes = firstPhases.map(\.count), phaseCount = sizes.reduce(0, +)
        var probeIndex: Int?, probeWidth = roi.width, probeHeight = roi.height
        if let probe = spectrumProbe {
            guard probe.level >= 0, probe.level < sizes.count else { throw BackendError.invalid("Spectrum probe level unavailable") }
            for _ in 0..<probe.level { probeWidth = max(1, probeWidth/2); probeHeight = max(1, probeHeight/2) }
            guard probe.x >= 0, probe.y >= 0, probe.x < probeWidth, probe.y < probeHeight else {
                throw BackendError.invalid("Spectrum probe outside ROI pyramid level")
            }
            probeIndex = sizes.prefix(probe.level).reduce(0,+) + probe.y * probeWidth + probe.x
        }
        let raw = try DiskRecords(url: directory.appendingPathComponent("raw"), recordBytes: first.data.count)
        let phase = try DiskRecords(url: directory.appendingPathComponent("phase"), recordBytes: phaseCount * 4)
        let filtered = try DiskRecords(url: directory.appendingPathComponent("filtered"), recordBytes: phaseCount * 4)
        var metadata: [(sequence: UInt64, timestamp: Double)] = []
        var inputSignal: [Double] = [], filteredSignal: [Double] = []
        func store(_ frame: InputFrame, phases: [[Float]]) throws {
            let flattened = phases.flatMap { $0 }
            try raw.write(frame.data, at: metadata.count)
            try phase.writeFloats(flattened, at: metadata.count)
            metadata.append((frame.sequence, frame.timestamp))
            if let probeIndex { inputSignal.append(Double(flattened[probeIndex])) }
        }
        try store(first, phases: firstPhases)
        while let frame = try readNext() {
            let previous = metadata.last!
            guard frame.width == first.width, frame.height == first.height,
                  frame.format == first.format, frame.bytesPerRow == first.bytesPerRow,
                  previous.sequence != UInt64.max, frame.sequence == previous.sequence + 1,
                  frame.timestamp.isFinite, abs((frame.timestamp-previous.timestamp)*configuration.fps-1) <= 0.01 else {
                throw BackendError.invalid("Offline clip must have fixed geometry/format, contiguous sequence and uniform capture time (1% tolerance); split or resample discontinuous clips")
            }
            try store(frame, phases: metal.extractPhases(frame))
        }
        let count = metadata.count
        if configuration.resolvedFilter == .zeroPhase && count < 3 {
            throw BackendError.invalid("Zero-phase filtering requires at least 3 frames")
        }
        if spectrumProbe != nil && count < 4 { throw BackendError.invalid("Spectrum requires at least 4 frames") }
        let padding = configuration.resolvedFilter == .zeroPhase
            ? Int(min(Double(count - 1), ceil(3 * (configuration.fps / configuration.lowHz)))) : 0
        let forward = try DiskRecords(url: directory.appendingPathComponent("forward"), recordBytes: phaseCount * 4)
        let startPhase = try phase.floats(0), endPhase = try phase.floats(count - 1)
        func extended(_ i: Int) throws -> [Float] {
            let source = i - padding
            if source < 0 {
                let reflected = try phase.floats(-source)
                return zip(startPhase, reflected).map { 2*$0 - $1 }
            }
            if source >= count {
                let reflected = try phase.floats(2*count-2-source)
                return zip(endPhase, reflected).map { 2*$0 - $1 }
            }
            return try phase.floats(source)
        }
        let extendedCount = count + 2 * padding
        var filter = PhaseIIR(coefficients: configuration.coefficients(), endpoint: try extended(0))
        for i in 0..<extendedCount {
            let value = filter.apply(try extended(i))
            if configuration.resolvedFilter == .causal { try filtered.writeFloats(value, at: i) }
            else { try forward.writeFloats(value, at: i) }
        }
        if configuration.resolvedFilter == .zeroPhase {
            filter = PhaseIIR(coefficients: configuration.coefficients(), endpoint: try forward.floats(extendedCount-1))
            for i in stride(from: extendedCount-1, through: 0, by: -1) {
                let value = filter.apply(try forward.floats(i))
                if i >= padding && i < padding + count { try filtered.writeFloats(value, at: i-padding) }
            }
        }
        metal.reset()
        for i in 0..<count {
            let flat = try filtered.floats(i)
            if let probeIndex { filteredSignal.append(Double(flat[probeIndex])) }
            var offset = 0
            let levels = sizes.map { size -> [Float] in
                defer { offset += size }
                return Array(flat[offset..<(offset+size)])
            }
            let source = InputFrame(data: try raw.read(i), width: first.width, height: first.height,
                bytesPerRow: first.bytesPerRow, format: first.format,
                sequence: metadata[i].sequence, timestamp: metadata[i].timestamp)
            try emit(metal.reconstruct(source, phases: levels))
        }
        var spectra: PhaseSpectra?
        if let probe = spectrumProbe {
            spectra = try PhaseSpectra(signal: "local pyramid phase (not calibrated displacement)", probe: probe,
                levelWidth: probeWidth, levelHeight: probeHeight, amplification: configuration.amplification,
                input: SpectrumAnalyzer.analyze(samples: inputSignal, fps: configuration.fps),
                filtered: SpectrumAnalyzer.analyze(samples: filteredSignal, fps: configuration.fps))
        }
        return OfflineReport(frameCount: count, sampleRate: configuration.fps,
            lowHz: configuration.lowHz, highHz: configuration.highHz, amplification: configuration.amplification,
            sourceWidth: first.width, sourceHeight: first.height, roi: roi,
            firstSequence: first.sequence, firstTimestamp: first.timestamp, temporalFilter: configuration.resolvedFilter,
            paddingFramesPerEnd: padding, processingSeconds: ProcessInfo.processInfo.systemUptime-started, spectra: spectra)
    }
}
