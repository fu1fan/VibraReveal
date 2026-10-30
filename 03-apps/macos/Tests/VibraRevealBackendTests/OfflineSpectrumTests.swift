import XCTest
@testable import VibraRevealBackend

extension BackendTests {
    func testGain200AndFilterModeValidation() throws {
        try Configuration(fps: 120, lowHz: 5, highHz: 12, amplification: 200).validate()
        XCTAssertThrowsError(try Configuration(fps: 120, lowHz: 5, highHz: 12, amplification: 200.01).validate())
        XCTAssertThrowsError(try Configuration(fps: 120, lowHz: 5, highHz: 12, temporalFilter: .zeroPhase).validate())
        let offline = try engine(Configuration(fps: 120, lowHz: 5, highHz: 12, mode: .offline))
        XCTAssertThrowsError(try offline.process(frame())) // Cannot silently use causal for a zero-phase request.
        let realtime = try engine(config(alpha: 200))
        for i in 0..<12 {
            let result = try realtime.process(frame(sequence: UInt64(i), value: { x,y in
                Float(0.5 + 0.25*sin(Double(x)*0.4 + 0.03*sin(Double(i))))
            }))
            XCTAssertTrue(result.pixels.allSatisfy(\.isFinite))
        }
    }

    func testSpectrumNormalizationAndNyquist() throws {
        let count = 256, fps = 128.0, amplitude = 0.25
        let signal = (0..<count).map { 2 + amplitude*sin(2*Double.pi*8*Double($0)/fps) }
        let spectrum = try SpectrumAnalyzer.analyze(samples: signal, fps: fps)
        XCTAssertEqual(spectrum.peakFrequencyHz, 8)
        XCTAssertEqual(spectrum.amplitude[16], amplitude, accuracy: 1e-12)
        XCTAssertEqual(spectrum.powerSpectralDensity.reduce(0,+)*spectrum.binSpacingHz,
                       amplitude*amplitude/2, accuracy: 1e-12)
        let nyquist = try SpectrumAnalyzer.analyze(samples: (0..<count).map { $0%2 == 0 ? 0.4 : -0.4 }, fps: fps)
        XCTAssertEqual(nyquist.amplitude[count/2], 0.4, accuracy: 1e-12)
        XCTAssertEqual(nyquist.powerSpectralDensity.reduce(0,+)*nyquist.binSpacingHz, 0.16, accuracy: 1e-12)
        let zero = try SpectrumAnalyzer.analyze(samples: Array(repeating: 0, count: 19), fps: 59.94)
        XCTAssertNil(zero.peakFrequencyHz)
        XCTAssertEqual(zero.sampleCount, 19); XCTAssertEqual(zero.fftSize, 32)
        XCTAssertEqual(zero.observationResolutionHz, 59.94/19)
        XCTAssertThrowsError(try SpectrumAnalyzer.analyze(samples: [0,1,2], fps: fps))
        XCTAssertThrowsError(try SpectrumAnalyzer.analyze(samples: [0,1,2,.nan], fps: fps))
    }

    func testFFTMatchesIndependentDirectDFTForOddLength() throws {
        let signal: [Double] = [0.1,0.4,-0.2,0.9,-0.1,0.5,0.0,-0.3,0.7]
        let result = try SpectrumAnalyzer.analyze(samples: signal, fps: 59.94)
        let mean = signal.reduce(0,+)/Double(signal.count)
        let window = signal.indices.map { 0.5-0.5*cos(2*Double.pi*Double($0)/Double(signal.count)) }
        for k in result.frequencyHz.indices {
            var real = 0.0, imag = 0.0
            for i in signal.indices {
                let angle = -2*Double.pi*Double(k*i)/Double(result.fftSize)
                real += (signal[i]-mean)*window[i]*cos(angle)
                imag += (signal[i]-mean)*window[i]*sin(angle)
            }
            let factor = k == 0 || k == result.fftSize/2 ? 1.0 : 2.0
            XCTAssertEqual(result.amplitude[k], factor*sqrt(real*real+imag*imag)/window.reduce(0,+), accuracy: 1e-12)
        }
    }

    func testOfflineZeroPhaseIdentityStatic200AndCleanup() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        for gain: Float in [0, 200] {
            let cfg = Configuration(fps: 120, lowHz: 5, highHz: 12, amplification: gain,
                roi: Region(x: 2,y: 3,width: 17,height: 13), maxWorkingPixels: 256, mode: .offline)
            let processor = try OfflineMagnifier(configuration: cfg)
            var inputIndex = 0, outputCount = 0
            let report = try processor.process(scratchDirectory: directory,
                spectrumProbe: SpectrumProbe(x: 5,y: 5), readNext: {
                    guard inputIndex < 13 else { return nil }; defer { inputIndex += 1 }
                    return self.frame(width: 29, height: 21, sequence: UInt64(inputIndex), bits: 10)
                }, emit: { output in
                    XCTAssertEqual(output.sequence, UInt64(outputCount)); outputCount += 1
                    XCTAssertEqual(output.width, 17); XCTAssertEqual(output.height, 13)
                    XCTAssertEqual(output.temporalFilter, .zeroPhase)
                    let source = self.frame(width: 29, height: 21, bits: 10)
                    for y in 0..<13 { for x in 0..<17 {
                        let offset = ((y+3)*29+x+2)*2
                        let v = Float(UInt16(source.data[offset]) | UInt16(source.data[offset+1])<<8)/1023
                        XCTAssertEqual(output.pixels[y*17+x], v, accuracy: 3e-6)
                    } }
                })
            XCTAssertEqual(outputCount, 13); XCTAssertEqual(report.frameCount, 13)
            XCTAssertEqual(report.paddingFramesPerEnd, 12)
            XCTAssertNil(report.spectra?.filtered.peakFrequencyHz)
            XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: directory.path).isEmpty)
        }
    }

    func testOfflineZeroPhaseMovingTextureAndSpectrumPeak() throws {
        let processor = try OfflineMagnifier(configuration: Configuration(
            fps: 120, lowHz: 5, highHz: 15, amplification: 20, mode: .offline))
        var index = 0, sine = 0.0, cosine = 0.0, energy = 0.0
        func source(_ n: Int) -> InputFrame {
            let shift = 0.02*sin(2*Double.pi*10*Double(n)/120)
            return frame(width: 21,height: 17,sequence: UInt64(n),bits:16,value: { x,y in
                Float(0.5+0.3*sin((Double(x)+shift)*0.5)*cos(Double(y)*0.23))
            })
        }
        let report = try processor.process(spectrumProbe: SpectrumProbe(x: 7,y: 5), readNext: {
            guard index < 480 else { return nil }; defer { index += 1 }; return source(index)
        }, emit: { output in
            let n = Int(output.sequence)
            XCTAssertTrue(output.pixels.allSatisfy(\.isFinite))
            if n >= 120 && n < 360 {
                let input = source(n), p = 5*21+7
                let original = Double(UInt16(input.data[p*2]) | UInt16(input.data[p*2+1])<<8)/65535
                let added = Double(output.pixels[p])-original
                sine += added*sin(2*Double.pi*10*Double(n)/120)
                cosine += added*cos(2*Double.pi*10*Double(n)/120)
                energy += added*added
            }
        })
        XCTAssertGreaterThan(energy, 1e-6)
        XCTAssertLessThan(abs(cosine)/max(abs(sine),1e-12), 0.03, "Interior response should have no temporal phase lag")
        XCTAssertEqual(report.spectra!.input.peakFrequencyHz!, 10, accuracy: report.spectra!.input.binSpacingHz)
        XCTAssertEqual(report.spectra!.filtered.peakFrequencyHz!, 10, accuracy: report.spectra!.filtered.binSpacingHz)
    }

    func testOfflineZeroPhaseRejectsOutOfBandMotion() throws {
        let processor = try OfflineMagnifier(configuration: Configuration(
            fps: 128, lowHz: 5, highHz: 12, amplification: 5, mode: .offline))
        var index = 0
        let report = try processor.process(spectrumProbe: SpectrumProbe(x: 7,y: 5), readNext: {
            guard index < 512 else { return nil }
            let n = index; index += 1
            let t = Double(n)/128
            let shift = 0.02 * ([1.0,8.0,48.0].reduce(0) { $0 + sin(2*Double.pi*$1*t) })
            return self.frame(width:21,height:17,sequence:UInt64(n),timestamp:t,bits:16,value: { x,y in
                Float(0.5+0.3*sin((Double(x)+shift)*0.5)*cos(Double(y)*0.23))
            })
        }, emit: { _ in })
        let spectra = report.spectra!
        func ratio(_ hz: Double) -> Double {
            let bin = Int((hz / spectra.input.binSpacingHz).rounded())
            return spectra.filtered.amplitude[bin] / spectra.input.amplitude[bin]
        }
        XCTAssertGreaterThan(ratio(8), 0.9)
        XCTAssertLessThan(ratio(1), 0.05)
        XCTAssertLessThan(ratio(48), 0.05)
    }

    func testOfflineRejectsDiscontinuitiesAndCleansFailedJobs() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let processor = try OfflineMagnifier(configuration: Configuration(fps:120,lowHz:5,highHz:12,mode:.offline))
        var index = 0, emitted = 0
        XCTAssertThrowsError(try processor.process(scratchDirectory: directory, readNext: {
            defer { index += 1 }; return self.frame(sequence: UInt64(index*2))
        }, emit: { _ in emitted += 1 }))
        XCTAssertEqual(emitted, 0)
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: directory.path).isEmpty)
        index = 0
        XCTAssertThrowsError(try processor.process(scratchDirectory: directory, readNext: {
            guard index < 8 else { return nil }; defer { index += 1 }; return self.frame(sequence: UInt64(index))
        }, emit: { _ in throw BackendError.invalid("test sink failure") }))
        XCTAssertTrue(try FileManager.default.contentsOfDirectory(atPath: directory.path).isEmpty)
        index = 0
        XCTAssertThrowsError(try processor.process(scratchDirectory: directory, readNext: {
            guard index < 2 else { return nil }; defer { index += 1 }; return self.frame(sequence: UInt64(index))
        }, emit: { _ in XCTFail("Short zero-phase clip must not emit") }))
    }
}
