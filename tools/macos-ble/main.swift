import Foundation
import CoreBluetooth
import Darwin

let serviceID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e01")
let uptimeID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e02")
let echoID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e03")
let phyID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e04")

let usage = """
Usage: ble.sh scan [--name EXACT_NAME] [--id COREBLUETOOTH_UUID] [--timeout SECONDS] [--duplicates]
       ble.sh probe [--name EXACT_NAME] [--id COREBLUETOOTH_UUID] [--timeout SECONDS] [--write-hex HEX] [--phy 1m|2m|1m,2m,1m]
       ble.sh probe --phy 1m|2m --duration SECONDS --timeout SECONDS [--interval SECONDS]
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

    init(_ args: [String]) throws {
        guard let first = args.first, first == "scan" || first == "probe" else { throw CLIError(usage) }
        mode = first
        var i = 1
        while i < args.count {
            let option = args[i]
            if option == "--duplicates" { duplicates = true; i += 1; continue }
            guard i + 1 < args.count else { throw CLIError("missing value for \(option)") }
            let value = args[i + 1]
            switch option {
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
        if mode == "probe", name == nil, id == nil { name = "CH32V208-NIMBLE" }
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

final class BLESession: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
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

    init(options: Options) { self.options = options; super.init() }

    func start() {
        emit("start", ["mode": options.mode, "timeout_seconds": options.timeout,
                       "name_filter": options.name as Any? ?? NSNull(),
                       "id_filter": options.id?.uuidString as Any? ?? NSNull(),
                       "write_hex": options.write?.hex as Any? ?? NSNull(),
                       "requested_phys": options.phys, "duration_seconds": options.duration, "interval_seconds": options.interval])
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
                         "max_round_seconds": maxRoundSeconds])
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
        guard options.mode == "probe", matched, target == nil else { return }
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
        payload = options.write ?? (options.duration > 0 ? soakPayload(0) : nil)
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
        nextAction()
    }

    func nextAction() {
        guard !finished, let peripheral = target else { return }
        guard !actions.isEmpty else {
            if options.duration > 0 {
                let now = ProcessInfo.processInfo.systemUptime
                roundsCompleted += 1
                maxRoundSeconds = max(maxRoundSeconds, now - (roundStarted ?? now))
                let elapsed = now - (soakStarted ?? now)
                emit("soak_round", ["round": roundsCompleted, "elapsed_seconds": elapsed,
                                    "verified_bytes": verifiedBytes, "phy": currentPhy ?? 0])
                if elapsed >= options.duration {
                    finish(code: failures == 0 ? 0 : 5, reason: failures == 0 ? "soak_complete" : "soak_failed")
                    return
                }
                payload = options.write ?? soakPayload(roundsCompleted)
                phase = "between_rounds"
                DispatchQueue.main.asyncAfter(deadline: .now() + options.interval) { [weak self] in
                    guard let self = self, !self.finished else { return }
                    self.roundStarted = ProcessInfo.processInfo.systemUptime
                    self.actions = [("read_uptime", self.uptime!), ("write_echo", self.echo!),
                                    ("verify_echo", self.echo!), ("verify_phy", self.phy!)]
                    self.nextAction()
                }
                return
            }
            finish(code: failures == 0 ? 0 : 5, reason: failures == 0 ? "probe_complete" : "probe_failed"); return
        }
        let (action, characteristic) = actions.removeFirst()
        currentAction = action
        phase = action
        emit("gatt_operation", ["operation": action, "uuid": characteristic.uuid.uuidString])
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
            if options.duration > 0 && soakStarted == nil {
                soakStarted = ProcessInfo.processInfo.systemUptime
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
            emit("read", fields)
            if options.duration > 0 && failures > 0 {
                finish(code: 5, reason: "soak_data_verification_failed"); return
            }
        }
        nextAction()
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        lastProgress = ProcessInfo.processInfo.systemUptime
        guard !finished else { return }
        if let error = error {
            failures += 1
            emit("write_error", ["uuid": characteristic.uuid.uuidString, "message": error.localizedDescription])
            if options.duration > 0 { finish(code: 5, reason: "soak_write_error"); return }
        } else {
            let data = characteristic.uuid == phyID ? Data([currentPhy!]) : payload!
            emit("write", ["uuid": characteristic.uuid.uuidString, "hex": data.hex])
        }
        nextAction()
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
