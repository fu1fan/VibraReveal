import Foundation
import Metal

/// Serial synchronous processor. Use RealtimeMagnifier for concurrent camera producers.
public final class MetalMagnifier {
    public let deviceName: String
    public private(set) var configuration: Configuration
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private var kernels: [String: MTLComputePipelineState] = [:]
    private var controller: LoadController
    private struct Level {
        let width: Int, height: Int, smallWidth: Int, smallHeight: Int
        let temporary: MTLBuffer, blurred: MTLBuffer, small: MTLBuffer
        let band: MTLBuffer, amplified: MTLBuffer, reconstructed: MTLBuffer, history: MTLBuffer
    }
    private var levels: [Level] = []
    private var input: MTLBuffer?
    private var raw: MTLBuffer?
    private var shape: [Int] = []
    private var lastSequence: UInt64?
    private var lastTimestamp: Double?
    private var lastFormat: PixelFormat?

    public init(configuration: Configuration, device: MTLDevice? = MTLCreateSystemDefaultDevice()) throws {
        try configuration.validate()
        guard let device, let queue = device.makeCommandQueue() else {
            throw BackendError.unavailable("A Metal-capable macOS device is required")
        }
        self.device = device; self.queue = queue; self.deviceName = device.name
        self.configuration = configuration; self.controller = LoadController(configuration)
        guard let url = Bundle.module.url(forResource: "Motion", withExtension: "metal") else {
            throw BackendError.unavailable("Missing bundled Metal kernels")
        }
        let source = try String(contentsOf: url, encoding: .utf8)
        let library = try device.makeLibrary(source: source, options: nil)
        for name in ["ingest", "blur", "shrink", "subtract", "phase", "reconstruct"] {
            guard let function = library.makeFunction(name: name) else {
                throw BackendError.unavailable("Missing Metal function \(name)")
            }
            kernels[name] = try device.makeComputePipelineState(function: function)
        }
    }

    public func configure(_ configuration: Configuration) throws {
        try configuration.validate()
        self.configuration = configuration; controller = LoadController(configuration)
        levels = []; input = nil; raw = nil; shape = []; reset()
    }
    public func reset() { lastSequence = nil; lastTimestamp = nil; lastFormat = nil }

    private func buffer(_ bytes: Int) throws -> MTLBuffer {
        guard bytes <= device.maxBufferLength,
              let buffer = device.makeBuffer(length: bytes, options: .storageModeShared) else {
            throw BackendError.resource("Metal buffer allocation failed (\(bytes) bytes)")
        }
        return buffer
    }
    private func allocationBytes(width: Int, height: Int) -> Int {
        var w = width, h = height, total = w * h * 4
        for _ in 0..<configuration.levels {
            if w == 1 && h == 1 { break }
            let sw = max(1, w / 2), sh = max(1, h / 2)
            // 5 image buffers + 7 floats history; small image counted separately.
            total += w * h * 48 + sw * sh * 4
            w = sw; h = sh
        }
        return total
    }
    private func prepare(width: Int, height: Int) throws {
        levels = []; input = nil // Release the previous pyramid before allocation.
        input = try buffer(width * height * 4)
        var w = width, h = height
        for _ in 0..<configuration.levels {
            if w == 1 && h == 1 { break }
            let sw = max(1, w / 2), sh = max(1, h / 2), bytes = w * h * 4
            levels.append(try Level(width: w, height: h, smallWidth: sw, smallHeight: sh,
                temporary: buffer(bytes), blurred: buffer(bytes), small: buffer(sw * sh * 4),
                band: buffer(bytes), amplified: buffer(bytes), reconstructed: buffer(bytes), history: buffer(w * h * 28)))
            w = sw; h = sh
        }
    }
    private func encode(_ name: String, _ buffers: [MTLBuffer], _ parameters: [UInt32],
                        coefficients: [Float]? = nil, command: MTLCommandBuffer) throws {
        guard let encoder = command.makeComputeCommandEncoder(), let kernel = kernels[name] else {
            throw BackendError.gpu("Cannot create compute encoder")
        }
        encoder.setComputePipelineState(kernel)
        for (i, buffer) in buffers.enumerated() { encoder.setBuffer(buffer, offset: 0, index: i) }
        parameters.withUnsafeBytes { encoder.setBytes($0.baseAddress!, length: $0.count, index: buffers.count) }
        if let coefficients {
            coefficients.withUnsafeBytes { encoder.setBytes($0.baseAddress!, length: $0.count, index: buffers.count + 1) }
        }
        let tw = kernel.threadExecutionWidth
        let th = min(8, max(1, kernel.maxTotalThreadsPerThreadgroup / tw))
        encoder.dispatchThreads(MTLSize(width: Int(parameters[0]), height: Int(parameters[1]), depth: 1),
                                threadsPerThreadgroup: MTLSize(width: tw, height: th, depth: 1))
        encoder.endEncoding()
    }

    public func process(_ frame: InputFrame) throws -> OutputFrame {
        guard configuration.resolvedFilter == .causal else {
            throw BackendError.invalid("Zero-phase filtering needs a complete clip; use OfflineMagnifier.process")
        }
        let start = ProcessInfo.processInfo.systemUptime
        do { return try process(frame, start: start) }
        catch { reset(); throw error }
    }
    // Offline phase extraction/reconstruction share the exact spatial kernels with realtime.
    func extractPhases(_ frame: InputFrame) throws -> [[Float]] {
        _ = try process(frame, start: ProcessInfo.processInfo.systemUptime, phaseMode: 1)
        return levels.map { level in
            Array(UnsafeBufferPointer(start: level.amplified.contents().assumingMemoryBound(to: Float.self),
                                      count: level.width * level.height))
        }
    }
    func reconstruct(_ frame: InputFrame, phases: [[Float]]) throws -> OutputFrame {
        try process(frame, start: ProcessInfo.processInfo.systemUptime, phaseMode: 2, externalPhases: phases)
    }
    private func process(_ frame: InputFrame, start: Double, phaseMode: UInt32 = 0,
                         externalPhases: [[Float]]? = nil) throws -> OutputFrame {
        try frame.validate(maxBytes: configuration.mode == .offline ? device.maxBufferLength : configuration.maxInputBytes)
        let roi = configuration.roi ?? Region(x: 0, y: 0, width: frame.width, height: frame.height)
        try roi.validate(width: frame.width, height: frame.height)
        let scale = configuration.mode == .offline ? 1 : min(1, sqrt(Double(controller.pixelLimit) / Double(roi.width * roi.height)))
        var w = max(1, Int(Double(roi.width) * scale)), h = max(1, Int(Double(roi.height) * scale))
        // Thin ROIs can hit the 1-pixel axis floor; enforce the pixel cap again.
        if configuration.mode == .realtime && w * h > controller.pixelLimit {
            if w >= h { w = max(1, controller.pixelLimit / h) }
            else { h = max(1, controller.pixelLimit / w) }
        }
        // Hard memory admission applies even when adaptation is disabled.
        while configuration.mode == .realtime && allocationBytes(width: w, height: h) > configuration.maxWorkingBytes {
            w = max(1, w / 2); h = max(1, h / 2)
        }
        if configuration.mode == .offline {
            // Physical device admission only: never resize offline data to fit an application budget.
            let required = allocationBytes(width: w, height: h) + frame.data.count
            if device.recommendedMaxWorkingSetSize > 0 && UInt64(required) > device.recommendedMaxWorkingSetSize {
                throw BackendError.resource("Full-resolution offline processing needs \(required) GPU bytes, exceeding this device's recommended working set; no downscaling was performed")
            }
        }
        let currentShape = [frame.width, frame.height, roi.x, roi.y, roi.width, roi.height, w, h]
        var fresh = lastSequence == nil || shape != currentShape || lastFormat != frame.format
        if let lastSequence { fresh = fresh || lastSequence == UInt64.max || frame.sequence != lastSequence &+ 1 }
        if let lastTimestamp {
            let delta = frame.timestamp - lastTimestamp
            // Fixed-rate biquad: reject timebase drift/gaps instead of silently changing the frequency band.
            fresh = fresh || delta <= 0 || abs(delta * configuration.fps - 1) > 0.2
        }
        if shape != currentShape || input == nil {
            shape = []
            try prepare(width: w, height: h)
            shape = currentShape; fresh = true
        }
        if raw?.length != frame.data.count { raw = nil; raw = try buffer(frame.data.count) }
        guard let raw, let input, let command = queue.makeCommandBuffer() else {
            throw BackendError.gpu("Cannot allocate input or command buffer")
        }
        frame.data.withUnsafeBytes { raw.contents().copyMemory(from: $0.baseAddress!, byteCount: $0.count) }
        var p = [UInt32(w), UInt32(h), UInt32(frame.width), UInt32(frame.height),
                 UInt32(roi.x), UInt32(roi.y), UInt32(roi.width), UInt32(roi.height),
                 UInt32(frame.bytesPerRow), UInt32(frame.format.bytesPerPixel), UInt32(frame.format.bits),
                 fresh ? 1 : 0, 0, phaseMode]
        try encode("ingest", [raw, input], p, command: command)
        let c = configuration.coefficients()
        var current = input
        if let externalPhases {
            guard externalPhases.count == levels.count else { throw BackendError.invalid("Phase level count mismatch") }
            for (level, values) in zip(levels, externalPhases) {
                guard values.count == level.width * level.height else { throw BackendError.invalid("Phase size mismatch") }
                // Offline rotation reads only history.origin; causal history is not used in this path.
                let history = level.history.contents().assumingMemoryBound(to: Float.self)
                for j in values.indices { history[j * 7] = values[j] }
            }
        }
        for level in levels {
            p[0] = UInt32(level.width); p[1] = UInt32(level.height); p[12] = 0
            try encode("blur", [current, level.temporary], p, command: command)
            p[12] = 1
            try encode("blur", [level.temporary, level.blurred], p, command: command)
            p[0] = UInt32(level.smallWidth); p[1] = UInt32(level.smallHeight)
            p[2] = UInt32(level.width); p[3] = UInt32(level.height)
            try encode("shrink", [level.blurred, level.small], p, command: command)
            p[0] = UInt32(level.width); p[1] = UInt32(level.height)
            p[2] = UInt32(level.smallWidth); p[3] = UInt32(level.smallHeight)
            try encode("subtract", [current, level.small, level.band], p, command: command)
            try encode("phase", [level.band, level.amplified, level.history], p, coefficients: c, command: command)
            current = level.small
        }
        for level in (phaseMode == 1 ? [] : levels.reversed().map { $0 }) {
            p[0] = UInt32(level.width); p[1] = UInt32(level.height)
            p[2] = UInt32(level.smallWidth); p[3] = UInt32(level.smallHeight)
            try encode("reconstruct", [level.amplified, current, level.reconstructed], p, command: command)
            current = level.reconstructed
        }
        command.commit(); command.waitUntilCompleted()
        guard command.status == .completed else {
            throw BackendError.gpu(command.error?.localizedDescription ?? "Metal execution failed")
        }
        if phaseMode == 1 { current = input }
        let pixels = Array(UnsafeBufferPointer(start: current.contents().assumingMemoryBound(to: Float.self), count: w*h))
        lastSequence = frame.sequence; lastTimestamp = frame.timestamp; lastFormat = frame.format
        let elapsed = ProcessInfo.processInfo.systemUptime - start
        let performance = controller.observe(seconds: elapsed, actualPixels: w*h, config: configuration)
        return OutputFrame(temporalFilter: configuration.resolvedFilter, mode: configuration.mode, pixels: pixels, width: w, height: h, sourceWidth: frame.width,
                           sourceHeight: frame.height, roi: roi, sequence: frame.sequence,
                           timestamp: frame.timestamp, historyReset: fresh, performance: performance)
    }
}
