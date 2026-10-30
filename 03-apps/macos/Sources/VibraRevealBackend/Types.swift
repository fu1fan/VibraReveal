import Foundation

public enum BackendError: Error, CustomStringConvertible {
    case invalid(String), unavailable(String), resource(String), gpu(String)
    public var description: String {
        switch self {
        case .invalid(let s), .unavailable(let s), .resource(let s), .gpu(let s): return s
        }
    }
}

/// Unpacked grayscale. For RAW10/12, use gray16LE with right-aligned valid bits.
public enum PixelFormat: Equatable {
    case gray8
    case gray16LE(validBits: Int)
    var bytesPerPixel: Int { self == .gray8 ? 1 : 2 }
    var bits: Int {
        switch self { case .gray8: return 8; case .gray16LE(let n): return n }
    }
}

public struct Region: Equatable, Codable {
    public let x: Int, y: Int, width: Int, height: Int
    public init(x: Int, y: Int, width: Int, height: Int) {
        self.x = x; self.y = y; self.width = width; self.height = height
    }
    func validate(width: Int, height: Int) throws {
        guard x >= 0, y >= 0, self.width > 0, self.height > 0,
              x <= width, y <= height, self.width <= width - x, self.height <= height - y else {
            throw BackendError.invalid("ROI must be nonempty and inside the source frame")
        }
    }
}

/// timestamp: seconds in a single monotonic capture clock, NOT arrival/wall-clock time.
public struct InputFrame {
    public let data: Data
    public let width: Int, height: Int, bytesPerRow: Int
    public let format: PixelFormat
    public let sequence: UInt64
    public let timestamp: Double
    public init(data: Data, width: Int, height: Int, bytesPerRow: Int,
                format: PixelFormat, sequence: UInt64, timestamp: Double) {
        self.data = data; self.width = width; self.height = height
        self.bytesPerRow = bytesPerRow; self.format = format
        self.sequence = sequence; self.timestamp = timestamp
    }
    func validate(maxBytes: Int) throws {
        guard width > 0, height > 0, width <= 65535, height <= 65535,
              (1...16).contains(format.bits), timestamp.isFinite,
              bytesPerRow >= width * format.bytesPerPixel,
              bytesPerRow <= maxBytes / height,
              data.count == bytesPerRow * height, data.count <= maxBytes,
              data.count <= Int(UInt32.max) else {
            throw BackendError.invalid("Invalid dimensions, stride, bit depth, timestamp or frame byte count")
        }
    }
}

public enum ProcessingMode: String, Codable { case realtime, offline }

public enum TemporalFilter: String, Codable { case automatic, causal, zeroPhase }

public struct Configuration {
    public var temporalFilter: TemporalFilter
    public var resolvedFilter: TemporalFilter {
        temporalFilter == .automatic ? (mode == .offline ? .zeroPhase : .causal) : temporalFilter
    }
    public var mode: ProcessingMode
    public var fps: Double
    public var lowHz: Double
    public var highHz: Double
    public var amplification: Float
    public var levels: Int
    public var roi: Region?
    /// Initial working-pixel ceiling. The controller can reduce it, never enlarge the ROI.
    public var maxWorkingPixels: Int
    public var minWorkingPixels: Int
    public var maxInputBytes: Int
    public var maxWorkingBytes: Int
    /// Fraction of one source-frame period reserved for processing (including readback).
    public var utilization: Double
    public var adaptive: Bool
    public init(fps: Double, lowHz: Double, highHz: Double, amplification: Float = 10,
                levels: Int = 3, roi: Region? = nil, maxWorkingPixels: Int = 1_048_576,
                minWorkingPixels: Int = 256, maxInputBytes: Int = 256 * 1024 * 1024,
                maxWorkingBytes: Int = 256 * 1024 * 1024, utilization: Double = 0.75,
                adaptive: Bool = true, mode: ProcessingMode = .realtime,
                temporalFilter: TemporalFilter = .automatic) {
        self.temporalFilter = temporalFilter
        self.mode = mode
        self.fps = fps; self.lowHz = lowHz; self.highHz = highHz
        self.amplification = amplification; self.levels = levels; self.roi = roi
        self.maxWorkingPixels = maxWorkingPixels; self.minWorkingPixels = minWorkingPixels
        self.maxInputBytes = maxInputBytes; self.maxWorkingBytes = maxWorkingBytes
        self.utilization = utilization; self.adaptive = adaptive
    }
    func coefficients() -> [Float] {
        let low = lowHz / fps, high = highHz / fps
        let f0 = sqrt(low) * sqrt(high)
        let q = max(0.5, f0 / (high - low))
        let omega = 2 * Double.pi * f0
        let a = sin(omega) / (2 * q)
        return [Float(a / (1+a)), 0, Float(-a / (1+a)),
                Float(-2*cos(omega)/(1+a)), Float((1-a)/(1+a)), amplification]
    }
    public func validate() throws {
        guard fps.isFinite, lowHz.isFinite, highHz.isFinite, amplification.isFinite,
              0 < lowHz, lowHz < highHz, highHz < fps / 2,
              (0...200).contains(amplification), (1...8).contains(levels),
              minWorkingPixels > 0, maxWorkingPixels >= minWorkingPixels,
              maxWorkingPixels <= 65535 * 65535,
              maxInputBytes > 0, maxWorkingBytes >= 4096,
              utilization.isFinite, utilization > 0, utilization <= 1 else {
            throw BackendError.invalid("Require 0 < lowHz < highHz < fps/2, alpha 0...200, levels 1...8 and positive resource budgets")
        }
        guard mode == .offline || resolvedFilter == .causal else {
            throw BackendError.invalid("Zero-phase filtering requires offline mode")
        }
        let c = coefficients()
        // Jury stability conditions for the rounded Float32 denominator.
        guard c.allSatisfy(\.isFinite), c[0] > 0, abs(c[4]) < 1,
              1 + Double(c[3]) + Double(c[4]) > 0,
              1 - Double(c[3]) + Double(c[4]) > 0 else {
            throw BackendError.invalid("Frequency band is too narrow or low for stable Float32 filtering at this FPS")
        }
    }
}

public enum LoadStatus: String, Codable { case warmingUp, sustainable, reducingResolution, overloaded, offline }
public struct Performance: Codable {
    public let processingSeconds: Double
    public let smoothedSeconds: Double
    public let budgetSeconds: Double
    public let recommendedMaxFPS: Double
    public let status: LoadStatus
}
public struct OutputFrame {
    public let temporalFilter: TemporalFilter
    public let mode: ProcessingMode
    /// Normalized grayscale floats, row-major. Values are intentionally not clipped.
    public let pixels: [Float]
    public let width: Int, height: Int
    public let sourceWidth: Int, sourceHeight: Int
    public let roi: Region
    public let sequence: UInt64
    public let timestamp: Double
    public let historyReset: Bool
    public let performance: Performance
}

/// Conservative hysteresis: downscale only; recovery needs explicit reconfiguration.
struct LoadController {
    var pixelLimit: Int
    var smoothed = 0.0
    var samples = 0
    var overBudget = 0
    init(_ config: Configuration) { pixelLimit = config.maxWorkingPixels }
    mutating func observe(seconds: Double, actualPixels: Int, config: Configuration) -> Performance {
        samples += 1
        // Ignore startup/allocation outliers when deciding the steady-state pixel budget.
        smoothed = samples <= 4 ? seconds : 0.8 * smoothed + 0.2 * seconds
        let budget = config.utilization / config.fps
        overBudget = samples >= 4 && smoothed > budget ? overBudget + 1 : 0
        var status: LoadStatus = samples < 4 ? .warmingUp : .sustainable
        if samples >= 4 && smoothed > budget { status = .overloaded }
        if config.mode == .offline { status = .offline }
        if config.mode == .realtime && config.adaptive && samples >= 4 && overBudget >= 3 && actualPixels > config.minWorkingPixels {
            pixelLimit = max(config.minWorkingPixels, Int(Double(actualPixels) * 0.5))
            overBudget = 0; samples = 0; smoothed = 0
            status = .reducingResolution
        }
        return Performance(processingSeconds: seconds,
                           smoothedSeconds: smoothed == 0 ? seconds : smoothed,
                           budgetSeconds: config.mode == .offline ? 0 : budget,
                           recommendedMaxFPS: config.utilization / max(smoothed == 0 ? seconds : smoothed, 1e-9),
                           status: status)
    }
}
