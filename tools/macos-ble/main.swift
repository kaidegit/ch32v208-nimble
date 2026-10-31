import Foundation
import CoreBluetooth
import Darwin

let serviceID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e01")
let uptimeID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e02")
let echoID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e03")
let perfID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e05")
let bulkID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e06")
let phyID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e04")

let usage = """
Usage: ble.sh scan [--name EXACT_NAME] [--id COREBLUETOOTH_UUID] [--timeout SECONDS] [--duplicates]
       ble.sh probe [--name EXACT_NAME] [--id COREBLUETOOTH_UUID] [--timeout SECONDS] [--write-hex HEX] [--phy 1m|2m|1m,2m,1m]
       ble.sh probe --phy 1m|2m --duration SECONDS --timeout SECONDS [--interval SECONDS]
       ble.sh probe --perf [--direction up|down] [--phy 1m|2m] [--duration SECONDS] [--size 4..244]
       ble.sh l2cap [--phy 1m|2m] [--perf --direction up|down] [--duration SECONDS] [--size BYTES] [--psm 128]
       ble.sh probe|l2cap --echo-perf [--phy 1m|2m] [--duration SECONDS]
       ble.sh --self-test

scan defaults to all nearby BLE devices for 15 seconds. Filters only mark matching
advertisements; all discovered devices are logged. probe connects to one exact
match (default name CH32V208-NIMBLE), discovers GATT, and reads uptime and echo.
--write-hex optionally writes the test echo characteristic and verifies readback.
--phy requests matching TX/RX PHY through the board test service and checks the
negotiated PHY before and after the data operations; refused PHY is a failure.
A comma-separated PHY list repeats these operations within the same connection.
--duration keeps one connection active, repeatedly verifies varying 0..64-byte
payloads (or --write-hex), checks uptime and PHY, and fails on a 10-second stall.
--perf measures saturated one-way throughput (default up, 30s, 1M): GATT
writeWithoutResponse/notify uses 244-byte frames; CoC streams use 512-byte writes
without echo waits. Receiver sequence/pattern checks and final byte counts must
match. Summary payload_kbps counts one direction. --echo-perf retains the
stop-and-wait echo baseline (64-byte GATT, 4096-byte CoC rounds).
l2cap without perf checks 14 lengths (1..4096); --psm defaults to 128.
The timeout covers the entire scan/connect/read session. All output is JSONL.
CoreBluetooth exposes macOS UUIDs, not over-the-air BLE MAC addresses or raw PDUs.
"""

struct CLIError: Error, CustomStringConvertible {
    let description: String
    init(_ value: String) { description = value }
}

func parseHex(_ text: String) throws -> Data {
    let compact = text.filter { !$0.isWhitespace && $0 != ":" && $0 != "-" }
    guard compact.count % 2 == 0 else { throw CLIError("--write-hex requires complete byte pairs") }
    var result = Data()
    var index = compact.startIndex
    while index != compact.endIndex {
        let end = compact.index(index, offsetBy: 2)
        guard let byte = UInt8(compact[index..<end], radix: 16) else { throw CLIError("invalid hexadecimal byte") }
        result.append(byte)
        index = end
    }
    guard result.count <= 64 else { throw CLIError("echo buffer accepts at most 64 bytes") }
    return result
}

struct Options {
    var mode = "scan"
    var name: String?
    var id: UUID?
    var timeout = 15.0
    var duplicates = false
    var write: Data?
    var phys: [UInt8] = []
    var duration = 0.0
    var interval = 0.2
    var perf = false
    var echoPerf = false
    var direction = "up"
    var saturated: Bool { perf && !echoPerf }
    var size: Int?
    var psm: UInt16 = 128

    init(_ args: [String]) throws {
        guard let first = args.first, ["scan", "probe", "l2cap"].contains(first) else { throw CLIError(usage) }
        mode = first
        var i = 1
        while i < args.count {
            let option = args[i]
            if option == "--echo-perf" { perf = true; echoPerf = true; i += 1; continue }
            if option == "--perf" { perf = true; i += 1; continue }
            if option == "--duplicates" { duplicates = true; i += 1; continue }
            guard i + 1 < args.count else { throw CLIError("missing value for \(option)") }
            let value = args[i + 1]
            switch option {
            case "--direction":
                guard value == "up" || value == "down" else { throw CLIError("--direction requires up or down") }
                direction = value
            case "--size":
                guard let n = Int(value), (1...65536).contains(n) else { throw CLIError("--size must be in 1...65536") }
                size = n
            case "--psm":
                guard let n = UInt16(value), (128...255).contains(n) else { throw CLIError("--psm must be in 128...255") }
                psm = n
            case "--name":
                guard !value.isEmpty else { throw CLIError("--name must not be empty") }
                name = value
            case "--id":
                guard let uuid = UUID(uuidString: value) else { throw CLIError("--id must be a full CoreBluetooth UUID") }
                id = uuid
            case "--timeout":
                guard let seconds = Double(value), seconds.isFinite, seconds > 0, seconds <= 86400 else { throw CLIError("--timeout must be in (0, 86400] seconds") }
                timeout = seconds
            case "--duration":
                guard let seconds = Double(value), seconds.isFinite, seconds > 0, seconds <= 86360 else { throw CLIError("--duration must be in (0, 86360] seconds") }
                duration = seconds
            case "--interval":
                guard let seconds = Double(value), seconds.isFinite, seconds >= 0, seconds <= 10 else { throw CLIError("--interval must be in [0, 10] seconds") }
                interval = seconds
            case "--write-hex": write = try parseHex(value)
            case "--phy":
                let rates = value.split(separator: ",", omittingEmptySubsequences: false)
                guard !rates.isEmpty, rates.count <= 16,
                      rates.allSatisfy({ $0 == "1m" || $0 == "2m" }) else {
                    throw CLIError("--phy requires 1m or 2m, optionally comma-separated (up to16)")
                }
                phys = rates.map { $0 == "1m" ? 1 : 2 }
            default: throw CLIError("unknown option \(option)")
            }
            i += 2
        }
        if perf {
            if !args.contains("--duration") { duration = 30 }
            if !args.contains("--timeout") { timeout = duration + 30 }
            if !args.contains("--interval") { interval = 0 }
            if phys.isEmpty { phys = [1] }
        }
        if mode == "l2cap" && phys.isEmpty { phys = [1] }
        if mode == "scan" && (perf || size != nil || args.contains("--psm")) { throw CLIError("data options require probe or l2cap") }
        if mode != "l2cap" && args.contains("--psm") { throw CLIError("--psm requires l2cap") }
        if mode == "l2cap" && write != nil { throw CLIError("--write-hex requires probe") }
        if mode == "l2cap" && phys.count != 1 { throw CLIError("l2cap requires exactly one PHY") }
        if mode == "probe" && (size ?? 64) > (saturated ? 244 : 64) { throw CLIError("payload exceeds GATT test limit") }
        if saturated && mode == "probe" && (size ?? 244) < 4 { throw CLIError("GATT perf needs >=4 bytes for sequence") }
        if saturated && mode == "l2cap" && (size ?? 512) > 512 { throw CLIError("CoC perf SDU size must be <=512") }
        if saturated && duration > 3600 { throw CLIError("--perf duration must be <=3600 seconds") }
        if saturated && write != nil { throw CLIError("--write-hex requires echo test") }
        if args.contains("--direction") && !saturated { throw CLIError("--direction requires --perf") }
        if perf && write != nil && size != nil { throw CLIError("choose --size or --write-hex") }
        if mode != "scan", name == nil, id == nil { name = "CH32V208-NIMBLE" }
        if mode == "scan", write != nil || !phys.isEmpty || duration > 0 { throw CLIError("--write-hex, --phy and --duration require probe") }
        if duration > 0 && (phys.count != 1 || timeout < duration + 15) {
            throw CLIError("--duration requires exactly one PHY and --timeout at least duration+15")
        }
    }

    func matches(id candidate: UUID, localName: String?, peripheralName: String?) -> Bool {
        if let id = id, candidate != id { return false }
        if let name = name, localName != name && peripheralName != name { return false }
        return true
    }
}

func perfPayload(round: Int, size: Int) -> Data {
    Data((0..<size).map { UInt8(truncatingIfNeeded: round * 131 + $0 * 17 + $0 / 251) })
}

func matchesStreamBytes(_ bytes: Data, expected: Data, offset: Int, written: Int) -> Bool {
    guard offset >= 0, written <= expected.count, offset <= written,
          bytes.count <= written - offset else { return false }
    return bytes == expected.subdata(in: offset..<(offset + bytes.count))
}

func soakPayload(_ round: Int) -> Data {
    let lengths = [0, 1, 2, 3, 4, 5, 7, 16, 20, 23, 31, 32, 63, 64]
    return Data((0..<lengths[round % lengths.count]).map { UInt8(truncatingIfNeeded: round * 131 + $0 * 17) })
}

extension Data {
    var hex: String { map { String(format: "%02x", $0) }.joined() }
    var uint32LE: UInt32? {
        guard count == 4 else { return nil }
        return enumerated().reduce(UInt32(0)) { $0 | UInt32($1.element) << (8 * $1.offset) }
    }
}

func jsonValue(_ value: Any) -> Any {
    switch value {
    case let data as Data: return ["hex": data.hex, "length": data.count]
    case let uuid as CBUUID: return uuid.uuidString
    case let array as [Any]: return array.map(jsonValue)
    case let dict as [String: Any]: return dict.mapValues(jsonValue)
    case let dict as [CBUUID: Any]: return Dictionary(uniqueKeysWithValues: dict.map { ($0.key.uuidString, jsonValue($0.value)) })
    case is String, is NSNumber, is NSNull: return value
    default: return String(describing: value)
    }
}

func emit(_ event: String, _ fields: [String: Any] = [:]) {
    var payload = fields
    payload["event"] = event
    payload["time"] = ISO8601DateFormatter().string(from: Date())
    do {
        let data = try JSONSerialization.data(withJSONObject: payload.mapValues(jsonValue), options: [.sortedKeys, .fragmentsAllowed])
        FileHandle.standardOutput.write(data)
        FileHandle.standardOutput.write(Data([10]))
    } catch {
        FileHandle.standardError.write(Data("JSON encoding failed: \(error)\n".utf8))
    }
}

final class BLESession: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate, StreamDelegate {
    let options: Options
    let started = Date()
    var manager: CBCentralManager!
    var timer: Timer?
    var interrupt: DispatchSourceSignal?
    var seen = Set<UUID>()
    var matches = Set<UUID>()
    var advertisements = 0
    var target: CBPeripheral?
    var finished = false
    var pendingExit: (Int32, String)?
    var connected = false
    var startedScan = false
    var pendingServices = 0
    var characteristicCount = 0
    var uptime: CBCharacteristic?
    var echo: CBCharacteristic?
    var phy: CBCharacteristic?
    var perfControl: CBCharacteristic?
    var bulk: CBCharacteristic?
    var phyRequestedAt: Date?
    var pendingPhys: [UInt8] = []
    var currentPhy: UInt8?
    var actions: [(String, CBCharacteristic)] = []
    var currentAction: String?
    var failures = 0
    var phase = "initializing"
    var watchdog: Timer?
    var lastProgress = ProcessInfo.processInfo.systemUptime
    var soakStarted: TimeInterval?
    var roundStarted: TimeInterval?
    var roundsCompleted = 0
    var verifiedBytes = 0
    var maxRoundSeconds = 0.0
    var lastUptime: UInt32?
    var payload: Data?
    var channel: CBL2CAPChannel?
    var streamPayload = Data()
    var streamWritten = 0
    var streamReceived = 0
    var streamChunkEnd = 0
    var streamActive = false
    var transferStarted: TimeInterval?
    var transferEnded: TimeInterval?
    var bulkTimer: Timer?
    var bulkPhase = "idle"
    var bulkSent = 0
    var bulkBlocked = 0
    var bulkSequence = 0
    var bulkStatsPending = false
    var bulkLastData = 0.0
    var bulkMeasureSeconds: Double?
    var bulkReceiverStats: [String: Any] = [:]
    var bulkSize: Int { options.size ?? (options.mode == "probe" ? 244 : 512) }
    var measuredSeconds: Double {
        if let seconds = bulkMeasureSeconds { return seconds }
        guard let start = transferStarted else { return 0 }
        return (transferEnded ?? ProcessInfo.processInfo.systemUptime) - start
    }


    init(options: Options) { self.options = options; super.init() }

    func start() {
        emit("start", ["mode": options.mode, "timeout_seconds": options.timeout,
                       "name_filter": options.name as Any? ?? NSNull(),
                       "id_filter": options.id?.uuidString as Any? ?? NSNull(),
                       "write_hex": options.write?.hex as Any? ?? NSNull(),
                       "requested_phys": options.phys, "duration_seconds": options.duration, "interval_seconds": options.interval, "perf": options.perf, "size": options.size as Any? ?? NSNull(), "psm": options.psm])
        timer = Timer.scheduledTimer(withTimeInterval: options.timeout, repeats: false) { [weak self] _ in
            guard let self = self else { return }
            if !self.startedScan {
                emit("error", ["message": "Bluetooth initialization timed out. Check for a macOS Bluetooth permission prompt and rerun after granting access."])
                self.finish(code: 3, reason: "bluetooth_initialization_timeout")
            } else if self.options.mode == "scan" {
                self.finish(code: 0, reason: "scan_complete")
            } else {
                self.finish(code: 4, reason: self.target == nil ? "target_not_found" : "timeout_\(self.phase)")
            }
        }
        watchdog = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self = self, self.connected, !self.finished else { return }
            if ProcessInfo.processInfo.systemUptime - self.lastProgress > max(10, self.options.interval + 5) {
                self.finish(code: 5, reason: "operation_stalled_\(self.phase)")
            }
        }
        signal(SIGINT, SIG_IGN)
        let source = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        source.setEventHandler { [weak self] in self?.finish(code: 130, reason: "interrupted") }
        source.resume()
        interrupt = source
        manager = CBCentralManager(delegate: self, queue: nil, options: [CBCentralManagerOptionShowPowerAlertKey: false])
    }

    func finish(code: Int32, reason: String) {
        guard !finished else { return }
        finished = true
        if transferStarted != nil && transferEnded == nil { transferEnded = ProcessInfo.processInfo.systemUptime }
        if let channel = channel {
            for stream in [channel.inputStream as Stream, channel.outputStream as Stream] {
                stream.delegate = nil
                stream.close()
                stream.remove(from: .main, forMode: .default)
            }
            self.channel = nil
        }
        bulkTimer?.invalidate()
        watchdog?.invalidate()
        timer?.invalidate()
        manager?.stopScan()
        if let target = target, target.state == .connected {
            pendingExit = (code, reason)
            manager?.cancelPeripheralConnection(target)
            // Keep the run loop alive for the asynchronous disconnect.
            timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: false) { [weak self] _ in
                emit("disconnect_cleanup_timeout")
                self?.exitNow(code: code, reason: reason)
            }
            return
        }
        if let target = target, target.state != .disconnected {
            manager?.cancelPeripheralConnection(target)
        }
        exitNow(code: code, reason: reason)
    }

    func exitNow(code: Int32, reason: String) {
        timer?.invalidate()
        pendingExit = nil
        emit("summary", ["reason": reason, "exit_code": code,
                         "elapsed_seconds": Date().timeIntervalSince(started),
                         "advertisements": advertisements, "unique_devices": seen.count,
                         "matched_devices": matches.count, "connected": connected,
                         "characteristics": characteristicCount, "failures": failures,
                         "rounds_completed": roundsCompleted, "verified_bytes": verifiedBytes,
                         "soak_elapsed_seconds": soakStarted.map { ProcessInfo.processInfo.systemUptime - $0 } ?? 0,
                         "max_round_seconds": maxRoundSeconds,
                         "saturated": options.saturated, "direction": options.direction,
                         "queued_bytes": bulkSent, "backpressure_events": bulkBlocked,
                         "receiver_stats": bulkReceiverStats,
                         "transfer_seconds": max(0, measuredSeconds),
                         "payload_bytes_per_second": measuredSeconds > 0 ? Double(verifiedBytes) / measuredSeconds : 0,
                         "payload_kbps": measuredSeconds > 0 ? Double(verifiedBytes) * 8 / measuredSeconds / 1000 : 0,
                         "verified_echo_bytes_per_second": !options.saturated && measuredSeconds > 0 ? Double(verifiedBytes) / measuredSeconds : 0,
                         "aggregate_payload_kbps": measuredSeconds > 0 ? Double(verifiedBytes) * (options.saturated ? 8 : 16) / measuredSeconds / 1000 : 0])
        exit(code)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let states: [CBManagerState: String] = [.unknown: "unknown", .resetting: "resetting", .unsupported: "unsupported", .unauthorized: "unauthorized", .poweredOff: "powered_off", .poweredOn: "powered_on"]
        emit("bluetooth_state", ["state": states[central.state] ?? "unknown", "authorization": CBManager.authorization.rawValue])
        switch central.state {
        case .poweredOn:
            guard !startedScan else { return }
            startedScan = true
            phase = "scanning"
            central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: options.duplicates])
            emit("scan_started", ["all_services": true, "duplicates": options.duplicates])
        case .unauthorized:
            emit("error", ["message": "Bluetooth access denied or restricted. Allow WCH BLE Test or the launching app in System Settings > Privacy & Security > Bluetooth, then rerun."])
            finish(code: 3, reason: "bluetooth_unauthorized")
        case .poweredOff:
            emit("error", ["message": "Bluetooth is powered off. Turn Bluetooth on in macOS, then rerun."])
            finish(code: 3, reason: "bluetooth_powered_off")
        case .unsupported: finish(code: 3, reason: "bluetooth_unsupported")
        case .resetting:
            if startedScan { finish(code: 3, reason: "bluetooth_reset") }
        default: break
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let localName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        let matched = options.matches(id: peripheral.identifier, localName: localName, peripheralName: peripheral.name)
        advertisements += 1
        seen.insert(peripheral.identifier)
        if matched { matches.insert(peripheral.identifier) }
        emit("advertisement", ["id": peripheral.identifier.uuidString,
                               "name": localName ?? peripheral.name as Any? ?? NSNull(),
                               "local_name": localName as Any? ?? NSNull(),
                               "peripheral_name": peripheral.name as Any? ?? NSNull(),
                               "rssi": RSSI, "matched": matched, "advertisement": advertisementData])
        guard options.mode != "scan", matched, target == nil else { return }
        target = peripheral
        peripheral.delegate = self
        central.stopScan()
        phase = "connecting"
        emit("connecting", ["id": peripheral.identifier.uuidString])
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        lastProgress = ProcessInfo.processInfo.systemUptime
        connected = true
        phase = "discovering_services"
        emit("connected", ["id": peripheral.identifier.uuidString])
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        emit("error", ["message": error?.localizedDescription ?? "connection failed"])
        finish(code: 5, reason: "connect_failed")
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        if let (code, reason) = pendingExit {
            emit("disconnected", ["expected": true,
                                  "error": error?.localizedDescription as Any? ?? NSNull()])
            exitNow(code: code, reason: reason)
            return
        }
        guard !finished else { return }
        emit("disconnected", ["error": error?.localizedDescription as Any? ?? NSNull()])
        finish(code: 5, reason: "unexpected_disconnect")
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        lastProgress = ProcessInfo.processInfo.systemUptime
        if let error = error {
            emit("error", ["message": error.localizedDescription])
            finish(code: 5, reason: "service_discovery_failed"); return
        }
        let services = peripheral.services ?? []
        pendingServices = services.count
        for service in services {
            emit("service", ["uuid": service.uuid.uuidString, "primary": service.isPrimary])
            peripheral.discoverCharacteristics(nil, for: service)
        }
        phase = "discovering_characteristics"
        if pendingServices == 0 { prepareActions() }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        lastProgress = ProcessInfo.processInfo.systemUptime
        if let error = error {
            failures += 1
            emit("error", ["service": service.uuid.uuidString, "message": error.localizedDescription])
        }
        for characteristic in service.characteristics ?? [] {
            characteristicCount += 1
            emit("characteristic", ["service": service.uuid.uuidString, "uuid": characteristic.uuid.uuidString,
                                    "properties": characteristic.properties.rawValue])
            if service.uuid == serviceID {
                if characteristic.uuid == uptimeID { uptime = characteristic }
                if characteristic.uuid == echoID { echo = characteristic }
                if characteristic.uuid == phyID { phy = characteristic }
                if characteristic.uuid == perfID { perfControl = characteristic }
                if characteristic.uuid == bulkID { bulk = characteristic }
            }
        }
        pendingServices -= 1
        if pendingServices == 0 { prepareActions() }
    }

    func prepareActions() {
        guard let uptime = uptime, let echo = echo else {
            emit("error", ["message": "test service uptime/echo characteristics missing"])
            finish(code: 5, reason: "test_characteristics_missing"); return
        }
        guard uptime.properties.contains(.read), echo.properties.contains(.read) else {
            finish(code: 5, reason: "test_characteristics_not_readable"); return
        }
        payload = options.write ?? (options.perf || options.size != nil ? perfPayload(round: 0, size: options.size ?? 64) : (options.duration > 0 ? soakPayload(0) : nil))
        if payload != nil && !echo.properties.contains(.write) {
            finish(code: 5, reason: "echo_not_writable"); return
        }
        if !options.phys.isEmpty && (phy == nil || !phy!.properties.contains(.read) || !phy!.properties.contains(.write)) {
            emit("error", ["message": "PHY control characteristic missing or inaccessible; macOS can cache the old schema. See CH32_BLE_TEST_IDENTITY in the tool README."])
            finish(code: 5, reason: "phy_control_missing"); return
        }
        actions = []
        pendingPhys = options.phys
        let rounds = max(1, options.phys.count)
        for _ in 0..<rounds {
            if !options.phys.isEmpty { actions += [("set_phy", phy!), ("wait_phy", phy!)] }
            actions += [("read_uptime", uptime), ("read_echo", echo)]
            if payload != nil { actions += [("write_echo", echo), ("verify_echo", echo)] }
            if !options.phys.isEmpty { actions.append(("verify_phy", phy!)) }
        }
        if options.saturated {
            guard perfControl != nil, bulk != nil else { finish(code: 5, reason: "perf_characteristics_missing"); return }
            actions = [("set_phy", phy!), ("wait_phy", phy!)]
        } else if options.mode == "l2cap" {
            actions = [("set_phy", phy!), ("wait_phy", phy!)]
        } else if options.perf {
            actions = [("set_phy", phy!), ("wait_phy", phy!), ("write_echo", echo), ("verify_echo", echo)]
        }
        nextAction()
    }

    func nextAction() {
        guard !finished, let peripheral = target else { return }
        guard !actions.isEmpty else {
            if options.saturated {
                if bulkPhase == "done" { finish(code: 0, reason: "perf_complete") }
                else if bulkPhase == "idle" { prepareBulk() }
                return
            }
            if options.mode == "l2cap" {
                if transferEnded != nil { finish(code: failures == 0 ? 0 : 5, reason: "l2cap_complete"); return }
                phase = "opening_l2cap"
                peripheral.openL2CAPChannel(options.psm)
                return
            }
            if options.duration > 0 {
                if transferEnded != nil {
                    finish(code: failures == 0 ? 0 : 5, reason: failures == 0 ? "soak_complete" : "soak_failed")
                    return
                }
                let now = ProcessInfo.processInfo.systemUptime
                roundsCompleted += 1
                maxRoundSeconds = max(maxRoundSeconds, now - (roundStarted ?? now))
                let elapsed = now - (soakStarted ?? now)
                if !options.perf || roundsCompleted % 100 == 0 { emit("soak_round", ["round": roundsCompleted, "elapsed_seconds": elapsed,
                                    "verified_bytes": verifiedBytes, "phy": currentPhy ?? 0]) }
                if elapsed >= options.duration {
                    transferEnded = now
                    actions = [("verify_phy", phy!)]
                    nextAction()
                    return
                }
                payload = options.write ?? (options.perf || options.size != nil ? perfPayload(round: roundsCompleted, size: options.size ?? 64) : soakPayload(roundsCompleted))
                phase = "between_rounds"
                DispatchQueue.main.asyncAfter(deadline: .now() + options.interval) { [weak self] in
                    guard let self = self, !self.finished else { return }
                    self.roundStarted = ProcessInfo.processInfo.systemUptime
                    self.actions = [("read_uptime", self.uptime!), ("write_echo", self.echo!),
                                    ("verify_echo", self.echo!), ("verify_phy", self.phy!)]
                    if self.options.perf {
                        self.actions = [("write_echo", self.echo!), ("verify_echo", self.echo!)]
                    }
                    self.nextAction()
                }
                return
            }
            finish(code: failures == 0 ? 0 : 5, reason: failures == 0 ? "probe_complete" : "probe_failed"); return
        }
        let (action, characteristic) = actions.removeFirst()
        currentAction = action
        phase = action
        if !options.perf { emit("gatt_operation", ["operation": action, "uuid": characteristic.uuid.uuidString]) }
        if options.perf && !options.saturated && action == "write_echo" && transferStarted == nil {
            transferStarted = ProcessInfo.processInfo.systemUptime
            soakStarted = transferStarted
            roundStarted = transferStarted
        }
        if action == "set_phy" {
            currentPhy = pendingPhys.removeFirst()
            // Allow the central's automatic connection-setup PHY procedure
            // to settle before issuing an explicit test request.
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
                guard let self = self, !self.finished else { return }
                self.phyRequestedAt = Date()
                peripheral.writeValue(Data([self.currentPhy!]), for: characteristic, type: .withResponse)
            }
        } else if action == "write_echo" {
            peripheral.writeValue(payload!, for: characteristic, type: .withResponse)
        } else {
            peripheral.readValue(for: characteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        lastProgress = ProcessInfo.processInfo.systemUptime
        guard !finished else { return }
        if options.saturated && characteristic.uuid == bulkID {
            guard error == nil, let data = characteristic.value else { finish(code: 5, reason: "notification_error"); return }
            receiveBulk(data)
            return
        }
        if options.saturated && characteristic.uuid == perfID {
            receiveBulkStats(characteristic.value, error: error)
            return
        }
        if characteristic.uuid == phyID {
            guard error == nil, let data = characteristic.value, data.count == 6, data[0] == 1 else {
                emit("error", ["message": error?.localizedDescription ?? "invalid PHY status"])
                finish(code: 5, reason: "phy_status_failed"); return
            }
            emit("phy_status", ["operation": currentAction ?? "unknown", "tx_phy": data[1], "rx_phy": data[2],
                                "pending": data[3] != 0, "status": data[4], "requested_mask": data[5]])
            // A central can finish its own initial PHY procedure while our
            // request is queued. Wait for the requested result, not merely
            // the first successful Update Complete event after the write.
            if currentAction == "wait_phy", (data[3] != 0 || data[1] != currentPhy || data[2] != currentPhy),
               data[4] == 0 || data[4] == 0xff {
                guard Date().timeIntervalSince(phyRequestedAt ?? started) < 8 else {
                    finish(code: 5, reason: "phy_update_timeout"); return
                }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                    guard let self = self, !self.finished else { return }
                    peripheral.readValue(for: characteristic)
                }
                return
            }
            guard data[3] == 0, data[4] == 0, data[1] == currentPhy, data[2] == currentPhy else {
                finish(code: 5, reason: "phy_not_selected"); return
            }
            if options.duration > 0 && !options.perf && options.mode != "l2cap" && soakStarted == nil {
                soakStarted = ProcessInfo.processInfo.systemUptime
                transferStarted = soakStarted
                roundStarted = soakStarted
            }
            nextAction(); return
        }
        if let error = error {
            failures += 1
            emit("read_error", ["operation": currentAction ?? "unknown", "uuid": characteristic.uuid.uuidString, "message": error.localizedDescription])
            if options.duration > 0 { finish(code: 5, reason: "soak_read_error"); return }
        } else {
            let data = characteristic.value ?? Data()
            var fields: [String: Any] = ["operation": currentAction ?? "unknown", "uuid": characteristic.uuid.uuidString, "hex": data.hex, "length": data.count]
            if characteristic.uuid == uptimeID {
                if let seconds = data.uint32LE {
                    fields["uptime_seconds"] = seconds
                    if let previous = lastUptime, seconds < previous {
                        emit("error", ["message": "device uptime moved backwards", "previous": previous, "current": seconds])
                        finish(code: 5, reason: "device_uptime_reset"); return
                    }
                    lastUptime = seconds
                }
                else { failures += 1; fields["error"] = "uptime must contain 4 little-endian bytes" }
            }
            if currentAction == "verify_echo" {
                let valid = data == payload
                fields["verified"] = valid
                if !valid { failures += 1 } else { verifiedBytes += data.count }
            }
            if !options.perf { emit("read", fields) }
            if options.duration > 0 && failures > 0 {
                finish(code: 5, reason: "soak_data_verification_failed"); return
            }
        }
        nextAction()
    }

    func peripheral(_ peripheral: CBPeripheral, didOpen channel: CBL2CAPChannel?, error: Error?) {
        guard !finished else { return }
        lastProgress = ProcessInfo.processInfo.systemUptime
        guard error == nil, let channel = channel, channel.psm == options.psm else {
            emit("error", ["message": error?.localizedDescription ?? "invalid L2CAP channel"])
            finish(code: 5, reason: "l2cap_open_failed"); return
        }
        self.channel = channel
        emit("l2cap_open", ["psm": channel.psm, "write_window": 512])
        for stream in [channel.inputStream as Stream, channel.outputStream as Stream] {
            stream.delegate = self
            stream.schedule(in: .main, forMode: .default)
            stream.open()
        }
        if options.saturated { sendBulkControl(start: true) }
        else { startStreamRound() }
    }

    func startStreamRound() {
        guard !finished else { return }
        let lengths = [1, 2, 23, 127, 128, 129, 247, 251, 255, 511, 512, 513, 1024, 4096]
        streamPayload = perfPayload(round: roundsCompleted, size: options.size ?? (options.perf ? 4096 : lengths[roundsCompleted % lengths.count]))
        streamWritten = 0
        streamReceived = 0
        streamChunkEnd = min(512, streamPayload.count)
        streamActive = true
        phase = "l2cap_echo"
        let now = ProcessInfo.processInfo.systemUptime
        if transferStarted == nil { transferStarted = now; soakStarted = now }
        roundStarted = now
        pumpStream()
    }

    func pumpStream() {
        guard !finished, streamActive, let output = channel?.outputStream else { return }
        while streamWritten < streamChunkEnd && output.hasSpaceAvailable {
            let n = streamPayload.withUnsafeBytes { raw in
                output.write(raw.bindMemory(to: UInt8.self).baseAddress!.advanced(by: streamWritten), maxLength: streamChunkEnd - streamWritten)
            }
            if n < 0 { finish(code: 5, reason: "l2cap_write_error"); return }
            if n == 0 { return }
            streamWritten += n
            lastProgress = ProcessInfo.processInfo.systemUptime
        }
    }

    func stream(_ aStream: Stream, handle eventCode: Stream.Event) {
        guard !finished else { return }
        if eventCode.contains(.errorOccurred) || eventCode.contains(.endEncountered) {
            emit("error", ["message": aStream.streamError?.localizedDescription ?? "L2CAP stream ended"])
            finish(code: 5, reason: "l2cap_stream_closed"); return
        }
        if eventCode.contains(.hasSpaceAvailable) {
            if options.saturated { pumpBulk() } else { pumpStream() }
        }
        if eventCode.contains(.hasBytesAvailable), let input = channel?.inputStream, aStream === input {
            var buffer = [UInt8](repeating: 0, count: 4096)
            while input.hasBytesAvailable {
                let n = input.read(&buffer, maxLength: buffer.count)
                if n < 0 { finish(code: 5, reason: "l2cap_read_error"); return }
                if n == 0 { break }
                if options.saturated { receiveBulk(Data(buffer.prefix(n))); continue }
                guard streamActive, matchesStreamBytes(Data(buffer.prefix(n)), expected: streamPayload,
                                                       offset: streamReceived, written: streamWritten) else {
                    failures += 1
                    finish(code: 5, reason: "l2cap_echo_mismatch"); return
                }
                streamReceived += n
                lastProgress = ProcessInfo.processInfo.systemUptime
                if streamReceived == streamPayload.count {
                    streamActive = false
                    let now = ProcessInfo.processInfo.systemUptime
                    roundsCompleted += 1
                    verifiedBytes += streamPayload.count
                    maxRoundSeconds = max(maxRoundSeconds, now - (roundStarted ?? now))
                    if !options.perf || roundsCompleted % 100 == 0 {
                        emit("l2cap_round", ["round": roundsCompleted, "length": streamPayload.count, "verified_bytes": verifiedBytes])
                    }
                    if options.duration > 0 ? now - transferStarted! >= options.duration : roundsCompleted >= 14 {
                        transferEnded = now
                        actions = [("verify_phy", phy!)]
                        nextAction()
                    } else {
                        DispatchQueue.main.asyncAfter(deadline: .now() + options.interval) { [weak self] in self?.startStreamRound() }
                    }
                } else if streamReceived == streamChunkEnd {
                    streamChunkEnd = min(streamPayload.count, streamChunkEnd + 512)
                    pumpStream()
                }
            }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        lastProgress = ProcessInfo.processInfo.systemUptime
        guard !finished else { return }
        if options.saturated && characteristic.uuid == perfID {
            guard error == nil else { finish(code: 5, reason: "perf_control_write_failed"); return }
            if currentAction == "bulk_prepare" {
                DispatchQueue.main.asyncAfter(deadline: .now()+1) { [weak self] in
                    guard let self = self, !self.finished else { return }
                    self.configureBulkData()
                }
            } else if currentAction == "bulk_start" { beginBulkTransfer() }
            else { bulkPhase = "final"; requestBulkStats() }
            return
        }
        if let error = error {
            failures += 1
            emit("write_error", ["uuid": characteristic.uuid.uuidString, "message": error.localizedDescription])
            if options.duration > 0 { finish(code: 5, reason: "soak_write_error"); return }
        } else {
            let data = characteristic.uuid == phyID ? Data([currentPhy!]) : payload!
            if !options.perf { emit("write", ["uuid": characteristic.uuid.uuidString, "hex": data.hex]) }
        }
        nextAction()
    }
}


func bulkPattern(_ offset: Int) -> UInt8 { UInt8(truncatingIfNeeded: offset * 17 + offset / 251) }
func u32(_ n: Int) -> Data {
    Data((0..<4).map { UInt8(truncatingIfNeeded: n >> ($0 * 8)) })
}
func read32(_ d: Data, _ offset: Int) -> Int { Int(d.subdata(in: offset..<(offset+4)).uint32LE!) }

extension BLESession {
    func prepareBulk() {
        bulkPhase = "preparing"
        // Let the controller finish the PHY/DLE procedure before requesting
        // connection parameters; otherwise HCI may report collision (0x2a).
        DispatchQueue.main.asyncAfter(deadline: .now()+1) { [weak self] in
            guard let self = self, !self.finished else { return }
            self.sendBulkControl(start: false, prepare: true)
        }
    }
    func configureBulkData() {
        if options.mode == "l2cap" {
            phase = "opening_l2cap"
            target!.openL2CAPChannel(options.psm)
        } else if options.direction == "down" {
            target!.setNotifyValue(true, for: bulk!)
        } else {
            guard target!.maximumWriteValueLength(for: .withoutResponse) >= bulkSize else {
                finish(code: 5, reason: "gatt_write_size_exceeds_mtu"); return
            }
            sendBulkControl(start: true)
        }
    }
    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        guard !finished, options.saturated, characteristic.uuid == bulkID else { return }
        guard error == nil, characteristic.isNotifying else { finish(code: 5, reason: "subscribe_failed"); return }
        sendBulkControl(start: true)
    }
    func sendBulkControl(start: Bool, prepare: Bool = false) {
        var command = Data([1, options.mode == "probe" ? 1 : 2, options.direction == "up" ? 1 : 2, prepare ? 2 : (start ? 1 : 0),
                            UInt8(bulkSize & 255), UInt8(bulkSize >> 8), 0, 0])
        command.append(u32(Int(options.duration * 1000)))
        currentAction = prepare ? "bulk_prepare" : (start ? "bulk_start" : "bulk_stop")
        target!.writeValue(command, for: perfControl!, type: .withResponse)
    }
    func beginBulkTransfer() {
        bulkPhase = "transfer"
        phase = "bulk_\(options.direction)"
        if transferStarted == nil { transferStarted = ProcessInfo.processInfo.systemUptime; soakStarted = transferStarted }
        emit("perf_transfer", ["direction": options.direction, "payload_size": bulkSize,
                               "write_without_response": options.mode == "probe" && options.direction == "up"])
        bulkTimer = Timer.scheduledTimer(withTimeInterval: 0.01, repeats: true) { [weak self] _ in
            guard let self = self, !self.finished else { return }
            if self.bulkPhase == "drain" || self.bulkPhase == "final" ||
               (self.options.direction == "down" && ProcessInfo.processInfo.systemUptime - self.transferStarted! >= self.options.duration) {
                self.requestBulkStats()
            } else if self.options.direction == "up" { self.pumpBulk() }
        }
        if options.direction == "up" { pumpBulk() }
    }
    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) { if options.saturated { pumpBulk() } }
    func pumpBulk() {
        guard !finished, options.saturated, options.direction == "up", bulkPhase == "transfer" else { return }
        guard ProcessInfo.processInfo.systemUptime - transferStarted! < options.duration else {
            bulkPhase = "drain"; requestBulkStats(); return
        }
        var budget = 32
        if options.mode == "probe" {
            while budget > 0 && target!.canSendWriteWithoutResponse {
                var data = u32(bulkSequence)
                data.append(contentsOf: (4..<bulkSize).map { UInt8(truncatingIfNeeded: bulkSequence * 131 + $0 * 17) })
                target!.writeValue(data, for: bulk!, type: .withoutResponse)
                bulkSequence += 1; bulkSent += data.count; budget -= 1
                lastProgress = ProcessInfo.processInfo.systemUptime
            }
            if !target!.canSendWriteWithoutResponse { bulkBlocked += 1 }
        } else if let output = channel?.outputStream {
            while budget > 0 && output.hasSpaceAvailable {
                let data = Data((0..<bulkSize).map { bulkPattern(bulkSent + $0) })
                let n = data.withUnsafeBytes { output.write($0.bindMemory(to: UInt8.self).baseAddress!, maxLength: data.count) }
                if n < 0 { finish(code: 5, reason: "bulk_stream_write_error"); return }
                if n == 0 { bulkBlocked += 1; return }
                bulkSent += n; budget -= 1
                lastProgress = ProcessInfo.processInfo.systemUptime
            }
            if !output.hasSpaceAvailable { bulkBlocked += 1 }
        }
        if budget == 0 { DispatchQueue.main.async { [weak self] in self?.pumpBulk() } }
    }
    func receiveBulk(_ data: Data) {
        guard options.direction == "down", !data.isEmpty else { finish(code: 5, reason: "unexpected_bulk_data"); return }
        let now = ProcessInfo.processInfo.systemUptime
        if transferStarted == nil { transferStarted = now; soakStarted = now }
        var valid: Bool
        if options.mode == "probe" {
            valid = data.count == bulkSize && data.count >= 4 && read32(data,0) == bulkSequence
            if valid { for i in 4..<data.count { if data[i] != UInt8(truncatingIfNeeded: bulkSequence * 131 + i * 17) { valid = false; break } } }
            bulkSequence += 1
        } else {
            valid = data.enumerated().allSatisfy { $0.element == bulkPattern(verifiedBytes + $0.offset) }
        }
        guard valid else { failures += 1; finish(code: 5, reason: "bulk_data_mismatch"); return }
        verifiedBytes += data.count; roundsCompleted += 1
        bulkLastData = now; lastProgress = now
    }
    func requestBulkStats() {
        guard !bulkStatsPending, !finished, bulkPhase != "stopping" else { return }
        bulkStatsPending = true
        target!.readValue(for: perfControl!)
    }
    func receiveBulkStats(_ value: Data?, error: Error?) {
        bulkStatsPending = false
        guard error == nil, let data = value, data.count == 100, data[0] == 4, data[1] == (options.mode == "probe" ? 1 : 2), data[2] == (options.direction == "up" ? 1 : 2) else { finish(code: 5, reason: "bulk_stats_invalid"); return }
        let count = read32(data,8), packets = read32(data,12), errors = read32(data,16), milliseconds = read32(data,20)
        bulkReceiverStats = ["bytes": count, "packets": packets, "errors": errors, "data_elapsed_ms": milliseconds,
                             "allocation_retries": read32(data,24), "msys_free": read32(data,28), "producer_done": data[4] != 0]
        let names = ["tx_good", "tx_fail", "tx_late", "rx_valid", "rx_crc_err", "rx_timeouts", "no_bufs", "hw_errs", "tx_data_pdus", "tx_md_pdus", "rx_data_pdus", "rx_md_pdus"]
        bulkReceiverStats["phy_delta"] = Dictionary(uniqueKeysWithValues: names.enumerated().map { ($0.element, read32(data,32+$0.offset*4)) })
        bulkReceiverStats["max_ll_tx_queue"] = read32(data,96)
        bulkReceiverStats["producer_blocked_cycles"] = read32(data,80)
        bulkReceiverStats["min_msys_free"] = read32(data,84)
        bulkReceiverStats["dle_tx_octets"] = Int(data[88])
        bulkReceiverStats["dle_rx_octets"] = Int(data[89])
        bulkReceiverStats["dle_tx_time_us"] = Int(data[90]) | Int(data[91]) << 8
        bulkReceiverStats["dle_rx_time_us"] = Int(data[92]) | Int(data[93]) << 8
        bulkReceiverStats["connection_interval_ms"] = Double(Int(data[94]) | Int(data[95]) << 8) * 1.25
        guard data[88] == 251, data[89] == 251 else { finish(code: 5, reason: "dle_not_251"); return }
        guard errors == 0 else { failures += errors; finish(code: 5, reason: "mcu_bulk_data_errors"); return }
        let matched = options.direction == "up" ? count == bulkSent : data[4] != 0 && count == verifiedBytes
        if !matched { return }
        if bulkPhase == "final" {
            if options.direction == "up" {
                verifiedBytes = count; roundsCompleted = packets
                bulkMeasureSeconds = Double(milliseconds) / 1000
            } else { bulkMeasureSeconds = bulkLastData - transferStarted! }
            guard verifiedBytes > 0, measuredSeconds > 0 else { finish(code: 5, reason: "bulk_empty_transfer"); return }
            bulkTimer?.invalidate()
            transferEnded = ProcessInfo.processInfo.systemUptime
            bulkPhase = "done"
            actions = [("verify_phy", phy!)]
            nextAction()
        } else {
            bulkPhase = "stopping"
            sendBulkControl(start: false)
        }
    }
}

func selfTest() throws {
    guard try parseHex("00:Ab cd-ff") == Data([0, 171, 205, 255]),
          Data([0x78, 0x56, 0x34, 0x12]).uint32LE == 0x12345678,
          Data([1]).uint32LE == nil else { throw CLIError("hex or little-endian decoding failed") }
    let scan = try Options(["scan"])
    let probe = try Options(["probe"])
    let identifier = UUID()
    guard scan.matches(id: identifier, localName: nil, peripheralName: nil),
          probe.matches(id: identifier, localName: "CH32V208-NIMBLE", peripheralName: nil),
          !probe.matches(id: identifier, localName: "CH32V208-NIMBLE-other", peripheralName: nil) else { throw CLIError("exact target filter failed") }
    let byID = try Options(["probe", "--id", identifier.uuidString, "--name", "board"])
    guard byID.matches(id: identifier, localName: "board", peripheralName: nil),
          !byID.matches(id: UUID(), localName: "board", peripheralName: nil),
          !byID.matches(id: identifier, localName: "other", peripheralName: nil) else { throw CLIError("combined filter failed") }
    guard try Options(["probe", "--phy", "2m"]).phys == [2],
          try Options(["probe", "--phy", "1m"]).phys == [1],
          try Options(["probe", "--phy", "1m,2m,1m"]).phys == [1,2,1] else { throw CLIError("PHY options failed") }
    for invalid in [["probe", "--phy", "1m,,2m"], ["scan", "--phy", "2m"], ["probe", "--phy", "coded"], ["scan", "--timeout", "nan"], ["scan", "--timeout", "0"], ["probe", "--id", "abc"], ["scan", "--write-hex", "00"], ["probe", "--write-hex", "0g"], ["probe", "--write-hex", "0"], ["probe", "--write-hex", String(repeating: "00", count: 65)]] {
        if (try? Options(invalid)) != nil { throw CLIError("accepted invalid options: \(invalid)") }
    }
    guard Set((0..<14).map { soakPayload($0).count }) == Set([0,1,2,3,4,5,7,16,20,23,31,32,63,64]),
          soakPayload(13) != soakPayload(27),
          try Options(["probe", "--phy", "2m", "--duration", "30", "--timeout", "50"]).duration == 30 else {
        throw CLIError("soak payload/options failed")
    }
    guard read32(u32(0x78563412), 0) == 0x78563412,
          bulkPattern(250) == UInt8(truncatingIfNeeded: 250*17),
          bulkPattern(251) == UInt8(truncatingIfNeeded: 251*17+1),
          try Options(["probe", "--perf"]).saturated,
          try Options(["probe", "--echo-perf"]).echoPerf,
          try Options(["l2cap", "--perf", "--direction", "down"]).direction == "down" else {
        throw CLIError("bulk framing/options failed")
    }
    for invalid in [["probe", "--perf", "--size", "3"], ["l2cap", "--perf", "--size", "513"],
                    ["probe", "--perf", "--direction", "both"], ["probe", "--direction", "up"],
                    ["probe", "--perf", "--duration", "3601"]] {
        if (try? Options(invalid)) != nil { throw CLIError("accepted invalid bulk options") }
    }
    let expected = perfPayload(round: 1, size: 1024)
    guard matchesStreamBytes(expected.subdata(in: 0..<17), expected: expected, offset: 0, written: 512),
          matchesStreamBytes(expected.subdata(in: 17..<512), expected: expected, offset: 17, written: 512),
          !matchesStreamBytes(expected.subdata(in: 17..<513), expected: expected, offset: 17, written: 512),
          !matchesStreamBytes(Data([expected[0] ^ 1]), expected: expected, offset: 0, written: 512),
          !matchesStreamBytes(expected, expected: expected, offset: 1024, written: 1024),
          !matchesStreamBytes(Data([0]), expected: expected, offset: -1, written: 512),
          expected != perfPayload(round: 2, size: 1024),
          try Options(["probe", "--perf"]).duration == 30,
          try Options(["probe", "--perf"]).interval == 0,
          try Options(["l2cap", "--echo-perf", "--size", "4096"]).size == 4096 else {
        throw CLIError("perf options or fragmented stream verification failed")
    }
    for invalid in [["scan", "--perf"], ["probe", "--perf", "--size", "245"],
                    ["l2cap", "--size", "0"], ["l2cap", "--size", "65537"],
                    ["l2cap", "--psm", "0"], ["probe", "--psm", "128"],
                    ["l2cap", "--write-hex", "00"], ["l2cap", "--phy", "1m,2m"],
                    ["probe", "--perf", "--size", "1", "--write-hex", "00"]] {
        if (try? Options(invalid)) != nil { throw CLIError("accepted invalid perf options: \(invalid)") }
    }
    for invalid in [["probe", "--duration", "30", "--timeout", "50"], ["probe", "--phy", "2m", "--duration", "30"], ["probe", "--phy", "1m,2m", "--duration", "30", "--timeout", "50"]] {
        if (try? Options(invalid)) != nil { throw CLIError("accepted invalid soak options") }
    }
    emit("self_test", ["passed": true])
}

do {
    let arguments = Array(CommandLine.arguments.dropFirst())
    if arguments == ["--help"] || arguments == ["-h"] { print(usage); exit(0) }
    if arguments == ["--self-test"] { try selfTest(); exit(0) }
    let session = BLESession(options: try Options(arguments))
    session.start()
    withExtendedLifetime(session) { RunLoop.main.run() }
} catch {
    emit("error", ["message": String(describing: error)])
    exit(2)
}
