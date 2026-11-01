import XCTest
import Foundation
import zlib
import CVRRawCodec
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
    func testCompressedArchiveLosslessAndCorruption() throws {
        for bits in [8,10,12,16] {
            // Padded rows and nonzero upper bits must survive exactly.
            let size=16416
            let pixels=Data((0..<size).map { UInt8(($0/2)%9+($0%2 == 0 ? 0 : 224)) })
            var scratch=Data(count: size), encoded=Data(count: size)
            let n=pixels.withUnsafeBytes { input in scratch.withUnsafeMutableBytes { tmp in
                encoded.withUnsafeMutableBytes { out in vr_raw_encode(input.baseAddress,size,UInt32(bits),tmp.baseAddress,out.baseAddress) }
            } }
            XCTAssertGreaterThan(n,0); XCTAssertLessThan(Int(n),size)
            encoded=encoded.prefix(Int(n))
            func h(_ kind: UInt64) -> Data {
                var b=header(kind: kind, sequence: kind == 3 ? 1 : (kind == 2 ? 71 : 0),
                             time: kind == 3 ? UInt64(size) : (kind == 2 ? 1_234_567 : 0),
                             data: kind == 2 ? pixels : Data(), flags: 8)
                func put(_ offset: Int,_ value: Int) { for i in 0..<4 { b[offset+i]=UInt8(truncatingIfNeeded:value>>(8*i)) } }
                put(16,63); put(20,114); put(24,144); put(28,bits); put(68,63); put(72,114)
                if kind == 2 { put(12,encoded.count); put(76,1) }
                return b
            }
            let good=h(1)+h(2)+encoded+h(3)
            try archive(good) { reader in
                XCTAssertTrue(reader.info.compressedArchive)
                let frame=try XCTUnwrap(reader.readNext())
                XCTAssertEqual(frame.data,pixels); XCTAssertEqual(frame.sequence,71)
                XCTAssertNil(try reader.readNext()); XCTAssertEqual(reader.statistics.payloadBytes,UInt64(size))
            }
            // Re-recording must retain encoded bytes, not a raw payload under an encoded header.
            let destination=FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
            defer { try? FileManager.default.removeItem(at:destination) }
            try archive(good) { try $0.record(to:destination) }
            XCTAssertEqual(try Data(contentsOf:destination),good)
            for offset in [144,156,160,good.count-48] {
                var bad=good; bad[offset]^=2
                try archive(bad) { reader in XCTAssertThrowsError(try { while try reader.readNext() != nil {} }()) }
            }
            for bad in [Data(good.dropLast(80)),good+Data([0])] {
                try archive(bad) { reader in XCTAssertThrowsError(try { while try reader.readNext() != nil {} }()) }
            }
            var invalid=h(1); invalid[8]=9
            XCTAssertThrowsError(try FrameHeader(data:invalid))
        }
    }
    func testBoardCompressedFixtureWhenProvided() throws {
        guard let path=ProcessInfo.processInfo.environment["VR_TEST_COMPRESSED_ARCHIVE"] else { throw XCTSkip("Board fixture not supplied") }
        let reader=try FrameReader(archiveURL: URL(fileURLWithPath:path))
        XCTAssertTrue(reader.info.compressedArchive)
        while try reader.readNext() != nil {}
        XCTAssertEqual(reader.statistics.frames,8)
        XCTAssertEqual(reader.statistics.sequenceGaps,0)
        XCTAssertEqual(reader.statistics.payloadBytes,8*1280*800*2)
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
