import XCTest
import Foundation
import zlib
@testable import VibraRevealTransport

final class TransportTests: XCTestCase {
    func header(kind: UInt64, sequence: UInt64 = 0, time: UInt64 = 0, data: Data = Data(), flags: UInt64 = 0) -> Data {
        var b = Data(repeating: 0, count: 80)
        b.replaceSubrange(0..<4, with: Data("VRI1".utf8))
        func put(_ offset: Int, _ value: UInt64, _ bytes: Int) {
            for i in 0..<bytes { b[offset+i]=UInt8(truncatingIfNeeded: value >> (i*8)) }
        }
        put(4,1,2); put(6,kind,2); put(8,flags,4); put(12,UInt64(data.count),4)
        put(16,3,4); put(20,2,4); put(24,4,4); put(28,8,4)
        put(32,sequence,8); put(40,time,8); put(48,2997,4); put(52,100,4)
        let crc = data.withUnsafeBytes { crc32(0,$0.bindMemory(to: Bytef.self).baseAddress,uInt(data.count)) }
        put(64,UInt64(crc),4); put(68,3,4); put(72,2,4)
        return b
    }
    func archive(_ bytes: Data, _ body: (FrameReader) throws -> Void) throws {
        let url=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try bytes.write(to: url); defer { try? FileManager.default.removeItem(at: url) }
        try body(FrameReader(archiveURL: url))
    }
    func testPreservesPaddingFractionalRateAndCaptureMetadata() throws {
        let pixels=Data([1,2,3,0,4,5,6,0])
        let bytes=header(kind: 1)+header(kind: 2,sequence: 91,time: 123456789,data: pixels)+pixels+header(kind: 3,sequence: 1,time: 8)
        try archive(bytes) { reader in
            XCTAssertEqual(reader.info.fps,29.97,accuracy: 1e-10)
            let frame=try XCTUnwrap(reader.readNext())
            XCTAssertEqual(frame.sequence,91); XCTAssertEqual(frame.timestamp,0.123456789,accuracy: 1e-12)
            XCTAssertEqual(frame.bytesPerRow,4); XCTAssertEqual(frame.data,pixels)
            XCTAssertNil(try reader.readNext()); XCTAssertNil(try reader.readNext())
            XCTAssertEqual(reader.statistics.payloadBytes,8)
        }
    }
    func testRejectsCorruptionMissingEndWrongTotalsAndTrailingBytes() throws {
        let data=Data(repeating: 5,count: 8)
        let prefix=header(kind: 1)+header(kind: 2,sequence: 0,time: 1,data: data)
        for bad in [prefix+Data(repeating: 6,count: 8), prefix+data,
                    prefix+data+header(kind: 3,sequence: 2,time: 8),
                    prefix+data+header(kind: 3,sequence: 1,time: 8)+Data([0])] {
            try archive(bad) { reader in
                XCTAssertThrowsError(try { while try reader.readNext() != nil {} }())
                XCTAssertThrowsError(try reader.readNext())
            }
        }
    }
    func testRejectsOversizedFrameBeforeAllocation() throws {
        let data=Data(repeating: 0,count: 8)
        XCTAssertThrowsError(try FrameHeader(data: header(kind: 2,data: data),maxFrameBytes: 7))
        var invalid=header(kind: 2,data: data)
        invalid[12]=9
        XCTAssertThrowsError(try FrameHeader(data: invalid))
    }
    func testOfflineRejectsGapsRealtimeReportsThem() throws {
        let data=Data(repeating: 5,count: 8)
        for flags: UInt64 in [0,1,4,5] {
            let bytes=header(kind: 1,flags: flags)+header(kind: 2,sequence: 1,time: 1,data: data,flags: flags)+data
                + header(kind: 2,sequence: 3,time: 3,data: data,flags: flags)+data+header(kind: 3,sequence: 2,time: 16,flags: flags)
            try archive(bytes) { reader in
                XCTAssertNotNil(try reader.readNext())
                if flags != 1 { XCTAssertThrowsError(try reader.readNext()) }
                else {
                    XCTAssertNotNil(try reader.readNext()); XCTAssertNil(try reader.readNext())
                    XCTAssertEqual(reader.statistics.sequenceGaps,1)
                }
            }
        }
    }
    func testPipelinedRealtimeRejectsResizeAndEndDrops() throws {
        let pixels=Data(repeating: 5,count: 8)
        let hello=header(kind: 1,flags: 5)
        var resized=header(kind: 2,sequence: 0,time: 1,data: pixels,flags: 5)
        resized[16]=2
        try archive(hello+resized+pixels) { reader in
            XCTAssertTrue(reader.info.pipelined)
            XCTAssertThrowsError(try reader.readNext())
        }
        var end=header(kind: 3,sequence: 1,time: 8,flags: 5)
        end[56]=1
        try archive(hello+header(kind: 2,sequence: 0,time: 1,data: pixels,flags: 5)+pixels+end) { reader in
            XCTAssertNotNil(try reader.readNext())
            XCTAssertThrowsError(try reader.readNext())
        }
    }

}
