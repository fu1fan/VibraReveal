import Foundation
import Darwin
import zlib
import CVRBulk
import VibraRevealBackend

public enum TransportError: Error, CustomStringConvertible {
    case invalid(String), io(String)
    public var description: String {
        switch self { case .invalid(let s), .io(let s): return s }
    }
}

public struct FrameHeader {
    public static let size = 80
    public let kind: UInt64, flags: UInt64, payloadBytes: Int
    public let width: Int, height: Int, stride: Int, bits: Int
    public let sequence: UInt64, timestampNS: UInt64, dropped: UInt64
    public let fpsNumerator: UInt64, fpsDenominator: UInt64, checksum: UInt64
    public let sourceWidth: Int, sourceHeight: Int
    public var realtime: Bool { flags & 1 != 0 }
    public var pipelined: Bool { flags & 4 != 0 }
    public var syntheticTimestamps: Bool { flags & 2 != 0 }
    public var fps: Double { Double(fpsNumerator) / Double(fpsDenominator) }

    public init(data: Data, maxFrameBytes: Int = 256 * 1024 * 1024) throws {
        guard data.count == Self.size, data.prefix(4) == Data("VRI1".utf8) else {
            throw TransportError.invalid("Invalid VRI header")
        }
        func value(_ offset: Int, _ count: Int) -> UInt64 {
            var n: UInt64 = 0
            for i in 0..<count { n |= UInt64(data[data.startIndex + offset + i]) << (8 * i) }
            return n
        }
        guard value(4, 2) == 1, value(76, 4) == 0 else { throw TransportError.invalid("Unsupported VRI version/reserved fields") }
        kind=value(6,2); flags=value(8,4); payloadBytes=Int(value(12,4))
        width=Int(value(16,4)); height=Int(value(20,4)); stride=Int(value(24,4)); bits=Int(value(28,4))
        sequence=value(32,8); timestampNS=value(40,8)
        fpsNumerator=value(48,4); fpsDenominator=value(52,4); dropped=value(56,8); checksum=value(64,4)
        sourceWidth=Int(value(68,4)); sourceHeight=Int(value(72,4))
        guard (1...3).contains(kind), flags & ~7 == 0, fpsNumerator > 0, fpsDenominator > 0,
              fps > 0, fps <= 10000, (1...65535).contains(width), (1...65535).contains(height),
              (1...16).contains(bits), (width...65535).contains(sourceWidth), (height...65535).contains(sourceHeight),
              stride >= width * (bits == 8 ? 1 : 2), maxFrameBytes > 0,
              stride <= maxFrameBytes / height else { throw TransportError.invalid("Invalid VRI dimensions, rate or memory limit") }
        guard kind == 2 ? payloadBytes == stride * height : (payloadBytes == 0 && checksum == 0) else {
            throw TransportError.invalid("Invalid VRI payload length")
        }
    }
}

public struct TransportStatistics: Codable {
    public var firstSequence: UInt64?
    public var lastSequence: UInt64?
    public var firstTimestampNS: UInt64?
    public var lastTimestampNS: UInt64?
    public var frames: UInt64 = 0
    public var payloadBytes: UInt64 = 0
    public var sequenceGaps: UInt64 = 0
    public var senderQueueDropped: UInt64 = 0
    public var seconds: Double = 0
    public var sourceFPS: Double = 0
    public var syntheticTimestamps = false
    public var payloadMBps: Double { seconds > 0 ? Double(payloadBytes)/seconds/1e6 : 0 }
}

private final class Connection {
    let fd: Int32
    let timeout: Double
    private var closed = false
    init(host: String, port: UInt16, timeout: Double) throws {
        guard timeout.isFinite, timeout >= 1, timeout <= 3600 else { throw TransportError.invalid("Invalid timeout") }
        self.timeout = timeout
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET); address.sin_port = port.bigEndian
        guard inet_pton(AF_INET, host, &address.sin_addr) == 1 else { throw TransportError.invalid("Host must be numeric IPv4") }
        let socketFD = Darwin.socket(AF_INET, SOCK_STREAM, 0)
        guard socketFD >= 0 else { throw TransportError.io("socket failed") }
        fd = socketFD
        do {
            guard fcntl(fd, F_SETFL, O_NONBLOCK) == 0 else { throw TransportError.io("fcntl failed") }
            var yes: Int32 = 1
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout.size(ofValue: yes)))
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, socklen_t(MemoryLayout.size(ofValue: yes)))
            let result = withUnsafePointer(to: &address) { p in
                p.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
            }
            if result < 0 {
                guard errno == EINPROGRESS else { throw TransportError.io("connect: \(String(cString: strerror(errno)))") }
                try wait(sending: true, deadline: ProcessInfo.processInfo.systemUptime + timeout)
                var error: Int32 = 0, size = socklen_t(MemoryLayout<Int32>.size)
                guard getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0, error == 0 else {
                    throw TransportError.io("connect failed: \(String(cString: strerror(error)))")
                }
            }
        } catch { closed = true; Darwin.close(fd); throw error }
    }
    deinit { if !closed { Darwin.close(fd) } }
    private func wait(sending: Bool, deadline: Double) throws {
        while true {
            let remaining = deadline - ProcessInfo.processInfo.systemUptime
            guard remaining > 0 else { throw TransportError.io("Transport deadline exceeded") }
            var event = pollfd(fd: fd, events: Int16(sending ? POLLOUT : POLLIN), revents: 0)
            let n = Darwin.poll(&event, 1, Int32(ceil(remaining * 1000)))
            if n < 0 && errno == EINTR { continue }
            guard n > 0 else { throw TransportError.io("Transport timed out") }
            return
        }
    }
    func read(_ count: Int) throws -> Data {
        var data = Data(count: count)
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        try data.withUnsafeMutableBytes { buffer in
            var offset = 0
            while offset < count {
                try wait(sending: false, deadline: deadline)
                let n = Darwin.recv(fd, buffer.baseAddress!.advanced(by: offset), count-offset, 0)
                if n < 0 && [EINTR, EAGAIN, EWOULDBLOCK].contains(errno) { continue }
                guard n > 0 else { throw TransportError.io("Disconnected before verified end of stream") }
                offset += n
            }
        }
        return data
    }
    func write(_ data: Data) throws {
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        try data.withUnsafeBytes { buffer in
            var offset = 0
            while offset < data.count {
                try wait(sending: true, deadline: deadline)
                let n = Darwin.send(fd, buffer.baseAddress!.advanced(by: offset), data.count-offset, 0)
                if n < 0 && [EINTR, EAGAIN, EWOULDBLOCK].contains(errno) { continue }
                guard n > 0 else { throw TransportError.io("Acknowledgement failed") }
                offset += n
            }
        }
    }
}

private final class USBConnection {
    private let handle: OpaquePointer
    init(timeout: Double) throws {
        guard timeout.isFinite, timeout >= 1, timeout <= 3600 else { throw TransportError.invalid("Invalid USB timeout") }
        var error = [CChar](repeating: 0, count: 512)
        guard let h = vr_usb_open(timeout, &error, error.count) else { throw TransportError.io(String(cString: error)) }
        handle=h
    }
    deinit { vr_usb_close(handle) }
    func read(_ count: Int) throws -> Data {
        var data=Data(count: count)
        let result=data.withUnsafeMutableBytes { vr_usb_read(handle, $0.baseAddress, count) }
        guard result == 0 else { throw TransportError.io(String(cString: vr_usb_error(handle))) }
        return data
    }
    func write(_ data: Data) throws {
        let result=data.withUnsafeBytes { vr_usb_send(handle, $0.baseAddress, data.count) }
        guard result == 0 else { throw TransportError.io(String(cString: vr_usb_error(handle))) }
    }
}

/// Serial reader. CRC, lengths and end totals are checked before success.
/// Bulk uses bounded asynchronous USB queues without per-frame ACKs. The legacy
/// TCP realtime mode acknowledges frames. Downstream processing has its own queue.
public final class FrameReader {
    public let info: FrameHeader
    public private(set) var statistics = TransportStatistics()
    private let readBytes: (Int) throws -> Data
    private let acknowledge: (Data) throws -> Void
    private let maxFrameBytes: Int
    private let hello: Data
    private let archive: Bool
    private var complete = false
    private var failed = false
    private var previous: FrameHeader?
    private let started = ProcessInfo.processInfo.systemUptime
    private var packetSink: ((Data) throws -> Void)?
    private var finishSink: (() throws -> Void)?

    public convenience init(usbTimeout: Double = 30, maxFrameBytes: Int = 256 * 1024 * 1024) throws {
        let connection=try USBConnection(timeout: usbTimeout)
        try self.init(read: { try connection.read($0) }, ack: { try connection.write($0) },
                      archive: false, maxFrameBytes: maxFrameBytes)
        guard info.pipelined else { throw TransportError.invalid("USB sender did not negotiate pipelined transport") }
    }
    public convenience init(host: String, port: UInt16 = 5910, timeout: Double = 15,
                            maxFrameBytes: Int = 256 * 1024 * 1024) throws {
        let connection = try Connection(host: host, port: port, timeout: timeout)
        try self.init(read: { try connection.read($0) }, ack: { try connection.write($0) },
                      archive: false, maxFrameBytes: maxFrameBytes)
    }
    public convenience init(archiveURL: URL, maxFrameBytes: Int = 256 * 1024 * 1024) throws {
        let handle = try FileHandle(forReadingFrom: archiveURL)
        try self.init(read: { count in
            var data = Data()
            while data.count < count {
                guard let chunk = try handle.read(upToCount: count-data.count), !chunk.isEmpty else { break }
                data.append(chunk)
            }
            return data
        }, ack: { _ in }, archive: true, maxFrameBytes: maxFrameBytes)
    }
    private init(read: @escaping (Int) throws -> Data, ack: @escaping (Data) throws -> Void,
                 archive: Bool, maxFrameBytes: Int) throws {
        readBytes=read; acknowledge=ack; self.maxFrameBytes=maxFrameBytes; self.archive=archive
        hello=try read(FrameHeader.size)
        info=try FrameHeader(data: hello, maxFrameBytes: maxFrameBytes)
        guard info.kind == 1, info.sequence == 0, info.timestampNS == 0, info.dropped == 0 else {
            throw TransportError.invalid("Missing/invalid session hello")
        }
        statistics.sourceFPS=info.fps; statistics.syntheticTimestamps=info.syntheticTimestamps
    }
    public func readNext() throws -> InputFrame? {
        guard !failed else { throw TransportError.invalid("Reader has failed; reconnect with a new session") }
        if complete { return nil }
        do { return try readPacket() } catch { failed=true; throw error }
    }
    private func readPacket() throws -> InputFrame? {
        let raw = try readBytes(FrameHeader.size)
        let h = try FrameHeader(data: raw, maxFrameBytes: maxFrameBytes)
        guard h.kind != 1, h.flags == info.flags, h.fpsNumerator == info.fpsNumerator,
              h.fpsDenominator == info.fpsDenominator, h.bits == info.bits,
              h.sourceWidth == info.sourceWidth, h.sourceHeight == info.sourceHeight,
              h.dropped >= statistics.senderQueueDropped else { throw TransportError.invalid("Session metadata changed") }
        if h.kind == 3 {
            guard h.sequence == statistics.frames, h.timestampNS == statistics.payloadBytes else {
                throw TransportError.invalid("End frame/byte totals mismatch")
            }
            if (!info.realtime || info.pipelined) && h.dropped != 0 { throw TransportError.invalid("Lossless sender dropped frames") }
            if try archive && !readBytes(1).isEmpty { throw TransportError.invalid("Trailing bytes after archive end") }
            try packetSink?(raw)
            try finishSink?()
            try acknowledge(Data("VRDONE01".utf8))
            complete=true; statistics.senderQueueDropped=h.dropped
            statistics.seconds=ProcessInfo.processInfo.systemUptime-started
            return nil
        }
        if (!info.realtime || info.pipelined) && (h.width != info.width || h.height != info.height || h.stride != info.stride || h.dropped != 0) {
            throw TransportError.invalid("Lossless dimensions/drop counter changed")
        }
        if let p = previous {
            guard h.sequence > p.sequence, h.timestampNS > p.timestampNS else {
                throw TransportError.invalid("Non-increasing capture sequence/time")
            }
            let gap=h.sequence-p.sequence-1
            if (!info.realtime || info.pipelined) && gap != 0 { throw TransportError.invalid("Lossless sequence gap") }
            statistics.sequenceGaps += gap
        }
        let data = try readBytes(h.payloadBytes)
        guard data.count == h.payloadBytes else { throw TransportError.invalid("Truncated payload") }
        let checksum = data.withUnsafeBytes { crc32(0, $0.bindMemory(to: Bytef.self).baseAddress, uInt(data.count)) }
        guard UInt64(checksum) == h.checksum else { throw TransportError.invalid("Frame CRC32 mismatch") }
        try packetSink?(raw); try packetSink?(data)
        if info.realtime && !info.pipelined {
            var seq = h.sequence.littleEndian
            try withUnsafeBytes(of: &seq) { try acknowledge(Data($0)) }
        }
        if statistics.firstSequence == nil { statistics.firstSequence=h.sequence; statistics.firstTimestampNS=h.timestampNS }
        statistics.lastSequence=h.sequence; statistics.lastTimestampNS=h.timestampNS
        statistics.frames += 1; statistics.payloadBytes += UInt64(data.count)
        statistics.senderQueueDropped=h.dropped; previous=h
        return InputFrame(data: data, width: h.width, height: h.height, bytesPerRow: h.stride,
                          format: h.bits == 8 ? .gray8 : .gray16LE(validBits: h.bits),
                          sequence: h.sequence, timestamp: Double(h.timestampNS)/1e9)
    }

    /// Store exact transport bytes. Failed transfers remain clearly marked .partial.
    /// Archives preserve capture times, sequence gaps and resolution transitions.
    public func record(to url: URL) throws {
        guard statistics.frames == 0, !complete else { throw TransportError.invalid("Recording must start before reading frames") }
        let partial = url.appendingPathExtension("partial")
        guard !FileManager.default.fileExists(atPath: url.path) else { throw TransportError.io("Output already exists") }
        let fd = Darwin.open(partial.path, O_WRONLY | O_CREAT | O_EXCL, 0o600)
        guard fd >= 0 else { throw TransportError.io("Cannot create new partial recording") }
        let handle = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        defer { packetSink=nil; finishSink=nil; try? handle.close() }
        try handle.write(contentsOf: hello)
        packetSink = { try handle.write(contentsOf: $0) }
        finishSink = { try handle.synchronize() }
        while try readNext() != nil {}
        try handle.synchronize()
        try handle.close()
        // link() is an atomic, no-overwrite publication in the same directory.
        guard Darwin.link(partial.path, url.path) == 0 else { throw TransportError.io("Cannot publish recording") }
        try FileManager.default.removeItem(at: partial)
    }
}
