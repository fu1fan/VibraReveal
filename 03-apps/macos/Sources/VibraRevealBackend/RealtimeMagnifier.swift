import Foundation

public enum Submission {
    case accepted
    /// The pending (not in-flight) frame was replaced. Its sequence is returned.
    case replaced(sequence: UInt64)
    case rejected(String)
}
public struct StreamStatistics {
    public let submitted: UInt64, completed: UInt64, dropped: UInt64, failed: UInt64
}

/// One in-flight frame plus one latest pending frame. No unbounded task or output queue.
/// The result callback runs on the processing queue and must not call waitUntilIdle().
/// Slow consumers cause pending-frame replacement, visible through statistics and sequence gaps.
public final class RealtimeMagnifier {
    private let processor: MetalMagnifier
    private let queue = DispatchQueue(label: "VibraReveal.Metal", qos: .userInitiated)
    private let condition = NSCondition()
    private var pending: InputFrame?
    private var running = false
    private var accepting = true
    private var submitted: UInt64 = 0, completed: UInt64 = 0, dropped: UInt64 = 0, failed: UInt64 = 0
    private let handler: (Result<OutputFrame, Error>) -> Void
    public let deviceName: String

    public init(configuration: Configuration, handler: @escaping (Result<OutputFrame, Error>) -> Void) throws {
        guard configuration.mode == .realtime else {
            throw BackendError.invalid("Use MetalMagnifier.process sequentially for lossless offline processing")
        }
        processor = try MetalMagnifier(configuration: configuration)
        deviceName = processor.deviceName; self.handler = handler
    }
    @discardableResult public func submit(_ frame: InputFrame) -> Submission {
        do { try frame.validate(maxBytes: processor.configuration.maxInputBytes) }
        catch { return .rejected(String(describing: error)) }
        condition.lock(); defer { condition.unlock() }
        guard accepting else { return .rejected("Stream is closed") }
        submitted &+= 1
        let old = pending?.sequence
        if old != nil { dropped &+= 1 }
        pending = frame
        if !running {
            running = true
            queue.async { self.drain() }
        }
        return old.map { .replaced(sequence: $0) } ?? .accepted
    }
    private func drain() {
        while true {
            condition.lock()
            guard let frame = pending else {
                running = false; condition.broadcast(); condition.unlock(); return
            }
            pending = nil; condition.unlock()
            let result = Result { try processor.process(frame) }
            condition.lock()
            switch result { case .success: completed &+= 1; case .failure: failed &+= 1 }
            condition.unlock()
            handler(result)
        }
    }
    /// Stop accepting frames; already accepted work drains normally.
    public func close() { condition.lock(); accepting = false; condition.unlock() }
    /// Call close() first to guarantee no producer can extend the wait.
    public func waitUntilIdle() {
        dispatchPrecondition(condition: .notOnQueue(queue))
        condition.lock(); defer { condition.unlock() }
        while running { condition.wait() }
    }
    public var statistics: StreamStatistics {
        condition.lock(); defer { condition.unlock() }
        return StreamStatistics(submitted: submitted, completed: completed, dropped: dropped, failed: failed)
    }
}
