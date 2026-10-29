import Foundation
import CoreBluetooth
import Darwin

let serviceID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e01")
let uptimeID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e02")
let echoID = CBUUID(string: "7a02480e-6d4d-4e8e-b7a2-d1c4a2f13e03")

let usage = """
Usage: ble.sh scan [--name EXACT_NAME] [--id COREBLUETOOTH_UUID] [--timeout SECONDS] [--duplicates]
       ble.sh probe [--name EXACT_NAME] [--id COREBLUETOOTH_UUID] [--timeout SECONDS] [--write-hex HEX]
       ble.sh --self-test

scan defaults to all nearby BLE devices for 15 seconds. Filters only mark matching
advertisements; all discovered devices are logged. probe connects to one exact
match (default name CH32V208-NIMBLE), discovers GATT, and reads uptime and echo.
--write-hex optionally writes the test echo characteristic and verifies readback.
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
                guard let seconds = Double(value), seconds.isFinite, seconds > 0, seconds <= 3600 else { throw CLIError("--timeout must be in (0, 3600] seconds") }
                timeout = seconds
            case "--write-hex": write = try parseHex(value)
            default: throw CLIError("unknown option \(option)")
            }
            i += 2
        }
        if mode == "probe", name == nil, id == nil { name = "CH32V208-NIMBLE" }
        if mode == "scan", write != nil { throw CLIError("--write-hex requires probe") }
    }

    func matches(id candidate: UUID, localName: String?, peripheralName: String?) -> Bool {
        if let id = id, candidate != id { return false }
        if let name = name, localName != name && peripheralName != name { return false }
        return true
    }
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
    var actions: [(String, CBCharacteristic)] = []
    var currentAction: String?
    var failures = 0
    var phase = "initializing"

    init(options: Options) { self.options = options; super.init() }

    func start() {
        emit("start", ["mode": options.mode, "timeout_seconds": options.timeout,
                       "name_filter": options.name as Any? ?? NSNull(),
                       "id_filter": options.id?.uuidString as Any? ?? NSNull(),
                       "write_hex": options.write?.hex as Any? ?? NSNull()])
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
                         "characteristics": characteristicCount, "failures": failures])
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
        actions = [("read_uptime", uptime), ("read_echo", echo)]
        if options.write != nil {
            guard echo.properties.contains(.write) else { finish(code: 5, reason: "echo_not_writable"); return }
            actions.append(("write_echo", echo))
            actions.append(("verify_echo", echo))
        }
        nextAction()
    }

    func nextAction() {
        guard let peripheral = target else { return }
        guard !actions.isEmpty else { finish(code: failures == 0 ? 0 : 5, reason: failures == 0 ? "probe_complete" : "probe_failed"); return }
        let (action, characteristic) = actions.removeFirst()
        currentAction = action
        phase = action
        emit("gatt_operation", ["operation": action, "uuid": characteristic.uuid.uuidString])
        if action == "write_echo" {
            peripheral.writeValue(options.write!, for: characteristic, type: .withResponse)
        } else {
            peripheral.readValue(for: characteristic)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            failures += 1
            emit("read_error", ["operation": currentAction ?? "unknown", "uuid": characteristic.uuid.uuidString, "message": error.localizedDescription])
        } else {
            let data = characteristic.value ?? Data()
            var fields: [String: Any] = ["operation": currentAction ?? "unknown", "uuid": characteristic.uuid.uuidString, "hex": data.hex, "length": data.count]
            if characteristic.uuid == uptimeID {
                if let seconds = data.uint32LE { fields["uptime_seconds"] = seconds }
                else { failures += 1; fields["error"] = "uptime must contain 4 little-endian bytes" }
            }
            if currentAction == "verify_echo" {
                let valid = data == options.write
                fields["verified"] = valid
                if !valid { failures += 1 }
            }
            emit("read", fields)
        }
        nextAction()
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error {
            failures += 1
            emit("write_error", ["uuid": characteristic.uuid.uuidString, "message": error.localizedDescription])
        } else { emit("write", ["uuid": characteristic.uuid.uuidString, "hex": options.write!.hex]) }
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
    for invalid in [["scan", "--timeout", "nan"], ["scan", "--timeout", "0"], ["probe", "--id", "abc"], ["scan", "--write-hex", "00"], ["probe", "--write-hex", "0g"], ["probe", "--write-hex", "0"], ["probe", "--write-hex", String(repeating: "00", count: 65)]] {
        if (try? Options(invalid)) != nil { throw CLIError("accepted invalid options: \(invalid)") }
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
