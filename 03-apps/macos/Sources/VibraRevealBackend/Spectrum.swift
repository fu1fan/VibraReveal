import Foundation

public struct SpectrumResult: Codable {
    public let sampleRate: Double
    public let sampleCount: Int
    public let fftSize: Int
    public let binSpacingHz: Double
    /// Observation resolution; zero-padding does not improve this value.
    public let observationResolutionHz: Double
    public let window: String
    public let detrending: String
    public let amplitudeUnit: String
    public let psdUnit: String
    public let frequencyHz: [Double]
    public let amplitude: [Double]
    public let powerSpectralDensity: [Double]
    public let peakFrequencyHz: Double?
}

/// One-sided, mean-detrended, periodic-Hann spectrum. Amplitudes use coherent-gain
/// correction; PSD uses window-energy normalization. DC/Nyquist are not doubled.
public enum SpectrumAnalyzer {
    public static func analyze(samples: [Double], fps: Double, unit: String = "rad") throws -> SpectrumResult {
        guard samples.count >= 4, samples.allSatisfy(\.isFinite), fps.isFinite, fps > 0 else {
            throw BackendError.invalid("Spectrum requires at least 4 finite, uniformly sampled values and positive FPS")
        }
        let count = samples.count
        var size = 1
        while size < count {
            guard size <= Int.max / 2 else { throw BackendError.resource("FFT size overflow") }
            size *= 2
        }
        let mean = samples.reduce(0, +) / Double(count)
        var real = [Double](repeating: 0, count: size), imaginary = real
        var windowSum = 0.0, windowEnergy = 0.0
        for i in 0..<count {
            let window = 0.5 - 0.5 * cos(2 * Double.pi * Double(i) / Double(count))
            real[i] = (samples[i] - mean) * window
            windowSum += window; windowEnergy += window * window
        }
        // In-place radix-2 complex FFT, zero-padded to the next power of two.
        var j = 0
        for i in 1..<size {
            var bit = size >> 1
            while j & bit != 0 { j ^= bit; bit >>= 1 }
            j ^= bit
            if i < j { real.swapAt(i, j); imaginary.swapAt(i, j) }
        }
        var length = 2
        while length <= size {
            let angle = -2 * Double.pi / Double(length)
            let stepR = cos(angle), stepI = sin(angle)
            for start in stride(from: 0, to: size, by: length) {
                var wr = 1.0, wi = 0.0
                for k in 0..<(length / 2) {
                    let even = start + k, odd = even + length / 2
                    let tr = wr * real[odd] - wi * imaginary[odd]
                    let ti = wr * imaginary[odd] + wi * real[odd]
                    real[odd] = real[even] - tr; imaginary[odd] = imaginary[even] - ti
                    real[even] += tr; imaginary[even] += ti
                    let next = wr * stepR - wi * stepI
                    wi = wr * stepI + wi * stepR; wr = next
                }
            }
            if length == size { break }
            length *= 2
        }
        var frequencies: [Double] = [], amplitude: [Double] = [], psd: [Double] = []
        for bin in 0...(size / 2) {
            let factor = bin == 0 || bin == size / 2 ? 1.0 : 2.0
            let magnitudeSquared = real[bin]*real[bin] + imaginary[bin]*imaginary[bin]
            frequencies.append(Double(bin) * fps / Double(size))
            amplitude.append(factor * sqrt(magnitudeSquared) / windowSum)
            psd.append(factor * magnitudeSquared / (fps * windowEnergy))
        }
        guard amplitude.allSatisfy(\.isFinite), psd.allSatisfy(\.isFinite) else {
            throw BackendError.invalid("Spectrum values exceed numeric range")
        }
        let peak = (1..<amplitude.count).max { amplitude[$0] < amplitude[$1] }
        return SpectrumResult(sampleRate: fps, sampleCount: count, fftSize: size,
            binSpacingHz: fps / Double(size), observationResolutionHz: fps / Double(count),
            window: "periodic Hann", detrending: "mean", amplitudeUnit: unit, psdUnit: "\(unit)^2/Hz",
            frequencyHz: frequencies, amplitude: amplitude, powerSpectralDensity: psd,
            peakFrequencyHz: peak.flatMap { amplitude[$0] > 1e-12 ? frequencies[$0] : nil })
    }
}

/// Coordinates are local to the selected ROI at the chosen pyramid level (zero-based).
public struct SpectrumProbe: Codable {
    public let level: Int, x: Int, y: Int
    public init(level: Int = 0, x: Int, y: Int) { self.level = level; self.x = x; self.y = y }
}
public struct PhaseSpectra: Codable {
    public let signal: String
    public let probe: SpectrumProbe
    public let levelWidth: Int, levelHeight: Int
    public let amplification: Float
    public let input: SpectrumResult
    /// Filtered phase BEFORE multiplying by amplification, not physical displacement.
    public let filtered: SpectrumResult
}
