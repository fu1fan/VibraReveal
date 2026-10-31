import Foundation
import VibraRevealBackend
import VibraRevealTransport

/// Optional streaming JSONL. Does not retain an unbounded history for live sessions.
/// Completion time is sampled before telemetry/output I/O; sink duration is separate.
final class StreamTelemetry {
    private let file: FileHandle
    private let lock = NSLock()
    private var arrivals: [UInt64: Double] = [:]
    init(path: String) throws {
        let fd=Darwin.open(path, O_WRONLY | O_CREAT | O_EXCL, 0o600)
        guard fd >= 0 else { throw BackendError.invalid("Cannot create new telemetry file") }
        self.file=FileHandle(fileDescriptor: fd, closeOnDealloc: true)
    }
    deinit { try? file.close() }
    private func write(_ event: [String: Any]) throws {
        var data=try JSONSerialization.data(withJSONObject: event, options: [.sortedKeys])
        data.append(10); try file.write(contentsOf: data)
    }
    func receive(_ frame: InputFrame, at time: Double) throws {
        lock.lock(); defer { lock.unlock() }
        arrivals[frame.sequence]=time
        try write(["event":"received", "sequence":frame.sequence, "capture":frame.timestamp,
                   "received":time, "width":frame.width, "height":frame.height])
    }
    func drop(_ sequence: UInt64) throws {
        lock.lock(); defer { lock.unlock() }
        arrivals.removeValue(forKey: sequence)
        try write(["event":"dropped", "sequence":sequence])
    }
    func complete(_ frame: OutputFrame, at completed: Double, sinkSeconds: Double) throws {
        lock.lock(); defer { lock.unlock() }
        guard let received=arrivals.removeValue(forKey: frame.sequence) else {
            throw BackendError.invalid("Missing frame receipt in telemetry")
        }
        try write(["event":"processed", "sequence":frame.sequence, "capture":frame.timestamp,
                   "received":received, "completed":completed, "processingSeconds":frame.performance.processingSeconds,
                   "sinkSeconds":sinkSeconds, "width":frame.width, "height":frame.height,
                   "historyReset":frame.historyReset])
    }
    func finish(_ stats: StreamStatistics, transport: TransportStatistics?) throws {
        lock.lock(); defer { lock.unlock() }
        var event: [String:Any] = ["event":"summary", "submitted":stats.submitted,
            "completed":stats.completed, "dropped":stats.dropped, "failed":stats.failed]
        if let transport {
            event["transport"]=try JSONSerialization.jsonObject(with: JSONEncoder().encode(transport))
        }
        try write(event); try file.synchronize()
    }
}
