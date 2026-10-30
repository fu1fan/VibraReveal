import XCTest
import Metal
@testable import VibraRevealBackend

final class BackendTests: XCTestCase {
    func config(alpha: Float = 0, adaptive: Bool = false) -> Configuration {
        Configuration(fps: 120, lowHz: 5, highHz: 12, amplification: alpha,
                      maxWorkingPixels: 2_000_000, adaptive: adaptive)
    }
    func engine(_ configuration: Configuration? = nil) throws -> MetalMagnifier {
        guard MTLCreateSystemDefaultDevice() != nil else { throw XCTSkip("Metal device unavailable") }
        return try MetalMagnifier(configuration: configuration ?? config())
    }
    func frame(width: Int = 35, height: Int = 27, sequence: UInt64 = 0,
               timestamp: Double? = nil, stride: Int? = nil, bits: Int = 8,
               value: (Int, Int) -> Float = { x,y in Float((x*17+y*13)%256)/255 }) -> InputFrame {
        let bpp = bits == 8 ? 1 : 2, stride = stride ?? width * (bits == 8 ? 1 : 2)
        var data = Data(repeating: 0xA5, count: stride*height)
        data.withUnsafeMutableBytes { (bytes: UnsafeMutableRawBufferPointer) in
            for y in 0..<height { for x in 0..<width {
                let v = UInt16((max(0,min(1,value(x,y))) * Float((1<<bits)-1)).rounded())
                let offset = y*stride+x*bpp
                bytes[offset] = UInt8(truncatingIfNeeded: v)
                if bpp == 2 { bytes[offset+1] = UInt8(v>>8) }
            } }
        }
        return InputFrame(data: data, width: width, height: height, bytesPerRow: stride,
                          format: bits == 8 ? .gray8 : .gray16LE(validBits: bits),
                          sequence: sequence, timestamp: timestamp ?? Double(sequence)/120)
    }
    func testValidation() throws {
        for cfg in [Configuration(fps: 20, lowHz: 5, highHz: 12),
                    Configuration(fps: .nan, lowHz: 1, highHz: 2),
                    Configuration(fps: 120, lowHz: 0, highHz: 2),
                    Configuration(fps: 120, lowHz: 1, highHz: 2, amplification: .infinity)] {
            XCTAssertThrowsError(try cfg.validate())
        }
        let processor = try engine()
        XCTAssertThrowsError(try processor.process(InputFrame(data: Data([0]), width: 100, height: 100,
            bytesPerRow: 100, format: .gray8, sequence: 0, timestamp: 0)))
        var cfg = config(); cfg.roi = Region(x: Int.max, y: 0, width: 1, height: 1)
        try processor.configure(cfg)
        XCTAssertThrowsError(try processor.process(frame()))
    }
    func testAlphaZeroIdentityForOddTinyAndNarrowDimensions() throws {
        let processor = try engine()
        for (w,h) in [(35,27),(1,1),(1,19),(21,1),(2,3),(127,95)] {
            let input = frame(width: w, height: h)
            let result = try processor.process(input)
            XCTAssertEqual(result.width, w); XCTAssertEqual(result.height, h)
            for (i,pixel) in result.pixels.enumerated() {
                XCTAssertEqual(pixel, Float(input.data[i])/255, accuracy: 3e-6)
            }
        }
    }
    func testROIAndPaddedRAW10() throws {
        var cfg = config(); cfg.roi = Region(x: 3,y: 2,width: 17,height: 13)
        let processor = try engine(cfg)
        let input = frame(width: 31,height: 23,stride: 68,bits: 10)
        let output = try processor.process(input)
        XCTAssertEqual(output.width,17); XCTAssertEqual(output.height,13)
        for y in 0..<13 { for x in 0..<17 {
            let offset=(y+2)*68+(x+3)*2
            let expected = Float(UInt16(input.data[offset]) | UInt16(input.data[offset+1])<<8)/1023
            XCTAssertEqual(output.pixels[y*17+x],expected,accuracy:3e-6)
        } }
    }
    func testHistoryResetsForGapsTimebaseAndResolutionChanges() throws {
        let processor = try engine(config(alpha: 8))
        XCTAssertTrue(try processor.process(frame()).historyReset)
        XCTAssertFalse(try processor.process(frame(sequence: 1)).historyReset)
        XCTAssertTrue(try processor.process(frame(sequence: 3)).historyReset)
        XCTAssertFalse(try processor.process(frame(sequence: 4)).historyReset)
        XCTAssertTrue(try processor.process(frame(sequence: 5,timestamp: 0.2)).historyReset)
        XCTAssertTrue(try processor.process(frame(width: 37,sequence: 6)).historyReset)
        XCTAssertTrue(try processor.process(frame(sequence: 7,bits: 10)).historyReset)
        var invalid = config(); invalid.highHz = 60
        XCTAssertThrowsError(try processor.configure(invalid))
        XCTAssertFalse(try processor.process(frame(sequence: 8,bits: 10)).historyReset)
    }
    func testStationaryAndBlackStayStable() throws {
        let processor = try engine(config(alpha: 20))
        var first: [Float] = []
        for n in 0..<16 {
            let output = try processor.process(frame(sequence: UInt64(n)))
            if n == 0 { first = output.pixels }
            for (a,b) in zip(output.pixels,first) { XCTAssertEqual(a,b,accuracy:1e-6) }
        }
        processor.reset()
        let black = try processor.process(frame(value: { _,_ in 0 }))
        XCTAssertTrue(black.pixels.allSatisfy { abs($0)<1e-7 })
    }
    func testBoxDownsampleRejectsSpatialCheckerboard() throws {
        var cfg = config(); cfg.maxWorkingPixels = 256
        let processor = try engine(cfg)
        let output = try processor.process(frame(width: 64,height: 64,value: { x,y in (x+y)%2 == 0 ? 0 : 1 }))
        XCTAssertEqual(output.width,16); XCTAssertEqual(output.height,16)
        XCTAssertTrue(output.pixels.allSatisfy { abs($0-0.5)<1e-6 })
    }
    func testThinROIHonorsPixelLimitAndFractionalFrameRate() throws {
        var cfg = Configuration(fps: 59.94, lowHz: 2, highHz: 9, amplification: 0,
                                maxWorkingPixels: 256, adaptive: false)
        let processor = try engine(cfg)
        let output = try processor.process(frame(width: 1, height: 4095))
        XCTAssertLessThanOrEqual(output.width * output.height, 256)
        let next = try processor.process(frame(width: 1, height: 4095, sequence: 1, timestamp: 1/59.94))
        XCTAssertFalse(next.historyReset)
        cfg.lowHz = 1e-12
        XCTAssertThrowsError(try cfg.validate())
    }
    func testAdaptiveResizeResetsHistory() throws {
        var cfg = config(adaptive: true)
        cfg.utilization = 1e-9 // Deterministically force overload without hardware-speed assumptions.
        let processor = try engine(cfg)
        var previous: OutputFrame?
        var reduced = false
        for n in 0..<12 {
            let output = try processor.process(frame(width: 128, height: 96, sequence: UInt64(n)))
            if let old = previous, output.width < old.width {
                XCTAssertTrue(output.historyReset); reduced = true
            }
            previous = output
        }
        XCTAssertTrue(reduced)
    }
    func testMemoryBudgetLimitsPyramid() throws {
        var cfg = config(); cfg.maxWorkingBytes = 8192
        let output = try engine(cfg).process(frame(width: 127,height: 99))
        XCTAssertLessThan(output.width,127)
        XCTAssertTrue(output.pixels.allSatisfy(\.isFinite))
    }
    func testLoadControllerHysteresisAndFloor() {
        var cfg = config(adaptive: true); cfg.maxWorkingPixels = 4096; cfg.minWorkingPixels = 1024
        var controller = LoadController(cfg)
        var result: Performance?
        for _ in 0..<6 { result = controller.observe(seconds:0.02,actualPixels:4096,config:cfg) }
        XCTAssertEqual(result?.status,.reducingResolution)
        XCTAssertEqual(controller.pixelLimit,2048)
        for _ in 0..<6 { result = controller.observe(seconds:0.02,actualPixels:2048,config:cfg) }
        XCTAssertEqual(controller.pixelLimit,1024)
        for _ in 0..<6 { result = controller.observe(seconds:0.02,actualPixels:1024,config:cfg) }
        XCTAssertEqual(result?.status,.overloaded)
        for _ in 0..<100 { result = controller.observe(seconds:0.001,actualPixels:1024,config:cfg) }
        XCTAssertEqual(result?.status,.sustainable)
        XCTAssertEqual(controller.pixelLimit,1024)
    }
    func testOfflinePreservesResolutionDespiteRealtimeBudgetsAndOverload() throws {
        for fps in [0.25, 59.94, 10000.0] {
            let cfg = Configuration(fps: fps, lowHz: fps * 0.04, highHz: fps * 0.1,
                amplification: 0, maxWorkingPixels: 256, minWorkingPixels: 256,
                maxInputBytes: 1, maxWorkingBytes: 4096, utilization: 1e-9, mode: .offline, temporalFilter: .causal)
            let processor = try engine(cfg)
            for n in 0..<10 {
                let input = frame(width: 127, height: 95, sequence: UInt64(n), timestamp: Double(n)/fps)
                let output = try processor.process(input)
                XCTAssertEqual(output.width, 127); XCTAssertEqual(output.height, 95)
                XCTAssertEqual(output.mode, .offline)
                XCTAssertEqual(output.performance.status, .offline)
                XCTAssertEqual(output.performance.budgetSeconds, 0)
                XCTAssertEqual(output.historyReset, n == 0)
                XCTAssertEqual(output.pixels[42], Float(input.data[42])/255, accuracy: 3e-6)
            }
            XCTAssertThrowsError(try RealtimeMagnifier(configuration: cfg, handler: { _ in }))
        }
    }
    func testColdStartDoesNotForceResolutionReduction() {
        let cfg = config(adaptive: true)
        var controller = LoadController(cfg)
        _ = controller.observe(seconds: 0.5, actualPixels: 4096, config: cfg)
        for _ in 0..<20 {
            let result = controller.observe(seconds: 0.001, actualPixels: 4096, config: cfg)
            XCTAssertNotEqual(result.status, .reducingResolution)
        }
        XCTAssertEqual(controller.pixelLimit, cfg.maxWorkingPixels)
    }
    func testRealtimeQueueIsBoundedAndCloseDrains() throws {
        let entered = DispatchSemaphore(value:0), release = DispatchSemaphore(value:0)
        let lock = NSLock(); var outputs: [OutputFrame] = []
        let backend = try RealtimeMagnifier(configuration: config()) { result in
            if case .success(let output) = result {
                lock.lock(); outputs.append(output); let first = outputs.count == 1; lock.unlock()
                if first { entered.signal(); _ = release.wait(timeout:.now()+10) }
            }
        }
        backend.submit(frame())
        XCTAssertEqual(entered.wait(timeout:.now()+10),.success)
        for n in 1...100 { backend.submit(frame(sequence:UInt64(n))) }
        XCTAssertEqual(backend.statistics.dropped,99)
        backend.close(); release.signal(); backend.waitUntilIdle()
        lock.lock(); let results=outputs; lock.unlock()
        XCTAssertEqual(results.map(\.sequence),[0,100])
        XCTAssertTrue(results[1].historyReset)
        XCTAssertEqual(backend.statistics.completed,2)
        if case .rejected = backend.submit(frame(sequence:101)) {} else { XCTFail("closed stream accepted frame") }
    }
    func testTemporalBandPassRespondsToMovingTexture() throws {
        // Compare added signal against a zero-gain reconstruction of the exact same RAW16 input.
        // RMS is normalized by input displacement amplitude; frequency selection should favor 8 Hz.
        func response(_ hz: Double) throws -> Double {
            let processor = try engine(config(alpha: 5))
            let baseline = try engine()
            var energy = 0.0, count = 0
            for n in 0..<360 {
                let shift = 0.08*sin(2*Double.pi*hz*Double(n)/120)
                let input = frame(width:32,height:24,sequence:UInt64(n),bits:16,value: { x,y in
                    Float(0.5+0.3*sin((Double(x)+shift)*0.5)*cos(Double(y)*0.23))
                })
                let output = try processor.process(input), zero = try baseline.process(input)
                if n >= 240 {
                    for (a,b) in zip(output.pixels,zero.pixels) { energy += Double((a-b)*(a-b)); count += 1 }
                }
            }
            return sqrt(energy/Double(count))
        }
        let inside = try response(8), low = try response(0.5), high = try response(40)
        print("Temporal response RMS: 8Hz=\(inside), 0.5Hz=\(low), 40Hz=\(high)")
        XCTAssertGreaterThan(inside,0.001)
        XCTAssertGreaterThan(inside,low*3)
        XCTAssertGreaterThan(inside,high*3)
    }
}
