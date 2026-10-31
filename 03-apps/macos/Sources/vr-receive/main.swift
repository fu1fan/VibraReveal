import Foundation
import VibraRevealTransport

do {
    var args = Array(CommandLine.arguments.dropFirst())
    if let i=args.firstIndex(of: "--usb") { args.insert("true", at: i+1) }
    if args.isEmpty || args == ["--help"] {
        print("vr-receive --usb [--timeout 30] [--output capture.vri]\nTCP fallback: vr-receive --host 192.168.77.1 [--port 5910] [--timeout 15] [--output capture.vri]\nWithout --output, validates and discards frames for transport benchmarking.\nReplay/validate: vr-receive --archive capture.vri")
    } else {
        var options: [String: String] = [:]
        let allowed: Set<String> = ["--usb", "--host", "--port", "--timeout", "--output", "--archive"]
        guard args.count % 2 == 0 else { throw TransportError.invalid("Options need values") }
        for i in stride(from: 0, to: args.count, by: 2) {
            guard allowed.contains(args[i]), options[args[i]] == nil else { throw TransportError.invalid("Unknown/duplicate option") }
            options[args[i]]=args[i+1]
        }
        let reader: FrameReader
        if let archive = options["--archive"] {
            guard options.count == 1 else { throw TransportError.invalid("--archive cannot be combined with network/output options") }
            reader = try FrameReader(archiveURL: URL(fileURLWithPath: archive))
        } else if options["--usb"] == "true" {
            guard options["--host"] == nil, options["--port"] == nil,
                  let timeout=Double(options["--timeout"] ?? "30") else { throw TransportError.invalid("Choose USB or TCP") }
            reader=try FrameReader(usbTimeout: timeout)
        } else {
            guard let host = options["--host"], let port = UInt16(options["--port"] ?? "5910"), port > 0,
                  let timeout = Double(options["--timeout"] ?? "15") else { throw TransportError.invalid("Invalid host/port/timeout") }
            reader = try FrameReader(host: host, port: port, timeout: timeout)
        }
        if let output = options["--output"] { try reader.record(to: URL(fileURLWithPath: output)) }
        else { while try reader.readNext() != nil {} }
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        print(String(data: try encoder.encode(reader.statistics), encoding: .utf8)!)
        FileHandle.standardError.write(Data("payloadMBps=\(reader.statistics.payloadMBps)\n".utf8))
    }
} catch {
    FileHandle.standardError.write(Data("error: \(error)\n".utf8)); exit(1)
}
