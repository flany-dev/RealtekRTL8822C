// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 RealtekRTL8822C contributors.

import AppKit
import Foundation
import Security
import ServiceManagement

@_silgen_name("RTWClientSendCommand")
private func RTWClientSendCommand(_ command: UInt32,
                                  _ ssid: UnsafePointer<CChar>?,
                                  _ password: UnsafePointer<CChar>?,
                                  _ enabled: UInt32) -> Int32

@_silgen_name("RTWClientCopyProperty")
private func RTWClientCopyProperty(_ key: UnsafePointer<CChar>,
                                   _ output: UnsafeMutablePointer<CChar>,
                                   _ capacity: Int) -> Int32

@_silgen_name("RTWClientCopyReport")
private func RTWClientCopyReport(_ output: UnsafeMutablePointer<CChar>,
                                 _ capacity: Int,
                                 _ includeDebug: UInt32) -> Int32

private enum DriverCommand: UInt32 {
    case updateStatus = 1
    case scan = 2
    case connect = 3
    case disconnect = 4
    case setInterfaceEnabled = 5
}

private let ioSuccess: Int32 = 0
private let ioError = Int32(bitPattern: 0xe00002bc)
private let ioTimeout = Int32(bitPattern: 0xe00002d6)
private let ioAborted = Int32(bitPattern: 0xe00002eb)

private func ioReturnDescription(_ value: Int32) -> String {
    let code = String(format: "0x%08x", UInt32(bitPattern: value))
    switch UInt32(bitPattern: value) {
        case 0xe00002bc: return "General driver error (\(code))"
        case 0xe00002c0: return "Driver service not found (\(code))"
        case 0xe00002c1: return "Operation is not permitted (\(code))"
        case 0xe00002c2: return "Invalid command or parameter (\(code))"
        case 0xe00002c7: return "Operation is unsupported (\(code))"
        case 0xe00002ca: return "Wi-Fi hardware I/O error (\(code))"
        case 0xe00002d5: return "Another Wi-Fi operation is active (\(code))"
        case 0xe00002d6: return "Operation timed out (\(code))"
        case 0xe00002d7: return "Wi-Fi hardware is offline (\(code))"
        case 0xe00002d8: return "Wi-Fi is not ready (\(code))"
        case 0xe00002eb: return "Operation was cancelled (\(code))"
        default: return code
    }
}

private final class DriverClient: @unchecked Sendable {
    func send(_ command: DriverCommand, ssid: String = "",
              password: String = "", enabled: Bool = false) -> Int32 {
        ssid.withCString { ssidPointer in
            password.withCString { passwordPointer in
                RTWClientSendCommand(command.rawValue, ssidPointer,
                                     passwordPointer, enabled ? 1 : 0)
            }
        }
    }

    func property(_ key: String, capacity: Int = 65536) -> String? {
        var buffer = [CChar](repeating: 0, count: capacity)
        let result = key.withCString {
            RTWClientCopyProperty($0, &buffer, buffer.count)
        }
        guard result == ioSuccess else { return nil }
        return String(cString: buffer)
    }

    func report(includeDebug: Bool) -> String {
        var buffer = [CChar](repeating: 0, count: 512 * 1024)
        let result = RTWClientCopyReport(&buffer, buffer.count,
                                         includeDebug ? 1 : 0)
        guard result == ioSuccess else {
            return "Unable to read driver report (\(ioReturnDescription(result)))."
        }
        return String(cString: buffer)
    }
}

private enum KeychainStore {
    private static let service = "org.realtekrtl8822c.app.Menu.wpa2"

    static func password(for ssid: String) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess,
              let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func save(password: String, for ssid: String) {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid
        ]
        let attributes: [String: Any] = [
            kSecValueData as String: Data(password.utf8)
        ]
        let status = SecItemUpdate(base as CFDictionary,
                                   attributes as CFDictionary)
        if status == errSecItemNotFound {
            var item = base
            item[kSecValueData as String] = Data(password.utf8)
            SecItemAdd(item as CFDictionary, nil)
        }
    }

    static func removePassword(for ssid: String) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: ssid
        ]
        SecItemDelete(query as CFDictionary)
    }
}

private struct WirelessNetwork: Equatable {
    let ssid: String
    let securityName: String
    let channel: Int
    let rssi: Int
    let bssid: String

    var isSecure: Bool { securityName != "Open" }
}

private final class NetworkBox: NSObject {
    let network: WirelessNetwork
    init(_ network: WirelessNetwork) { self.network = network }
}

private enum WiFiStatusGlyph {
    static func image(level: Int) -> NSImage {
        let clamped = min(max(level, 0), 4)
        let image = NSImage(size: NSSize(width: 19, height: 19), flipped: false) {
            _ in
            func alpha(_ segment: Int) -> CGFloat {
                segment <= clamped ? 1.0 : 0.20
            }

            let center = NSPoint(x: 9.5, y: 3.7)
            for (segment, radius) in [CGFloat(3.6), 6.3, 9.0].enumerated() {
                NSColor.black.withAlphaComponent(alpha(segment + 2)).setStroke()
                let path = NSBezierPath()
                path.lineWidth = 1.85
                path.lineCapStyle = .round
                path.appendArc(withCenter: center, radius: radius,
                               startAngle: 36, endAngle: 144)
                path.stroke()
            }

            NSColor.black.withAlphaComponent(alpha(1)).setFill()
            NSBezierPath(ovalIn: NSRect(x: 7.9, y: 2.1,
                                       width: 3.2, height: 3.2)).fill()
            return true
        }
        image.isTemplate = true
        return image
    }

    static func level(for rssi: Int?) -> Int {
        guard let rssi else { return 4 }
        if rssi >= -50 { return 4 }
        if rssi >= -67 { return 3 }
        if rssi >= -80 { return 2 }
        return 1
    }
}

private func parseScanResults(_ text: String) -> [WirelessNetwork] {
    let pattern = #"^\s*-\s+(.*?)\s+\((Open|WPA2|WPA),\s*ch=(\d+)\)\s+\[Signal:\s*(-?\d+)dBm,\s*BSSID:\s*([0-9a-fA-F:]+)\]$"#
    guard let expression = try? NSRegularExpression(pattern: pattern) else {
        return []
    }
    var strongest: [String: WirelessNetwork] = [:]
    for line in text.split(separator: "\n", omittingEmptySubsequences: true) {
        let value = String(line)
        let range = NSRange(value.startIndex..<value.endIndex, in: value)
        guard let match = expression.firstMatch(in: value, range: range),
              match.numberOfRanges == 6,
              let ssidRange = Range(match.range(at: 1), in: value),
              let securityRange = Range(match.range(at: 2), in: value),
              let channelRange = Range(match.range(at: 3), in: value),
              let rssiRange = Range(match.range(at: 4), in: value),
              let bssidRange = Range(match.range(at: 5), in: value),
              let channel = Int(value[channelRange]),
              let rssi = Int(value[rssiRange]) else { continue }
        let network = WirelessNetwork(
            ssid: String(value[ssidRange]),
            securityName: String(value[securityRange]),
            channel: channel,
            rssi: rssi,
            bssid: String(value[bssidRange]))
        guard !network.ssid.isEmpty else { continue }
        if strongest[network.ssid] == nil ||
            network.rssi > strongest[network.ssid]!.rssi {
            strongest[network.ssid] = network
        }
    }
    return strongest.values.sorted {
        if $0.rssi != $1.rssi { return $0.rssi > $1.rssi }
        return $0.ssid.localizedCaseInsensitiveCompare($1.ssid) == .orderedAscending
    }
}

#if RTW_APP_DEBUG
@MainActor
private final class DebugWindowController: NSWindowController {
    private let textView = NSTextView()
    private let client: DriverClient

    init(client: DriverClient) {
        self.client = client
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 820, height: 600),
            styleMask: [.titled, .closable, .resizable, .miniaturizable],
            backing: .buffered, defer: false)
        window.title = "Realtek RTL8822C Debug Info"
        window.center()
        super.init(window: window)
        buildContent()
        refresh()
    }

    required init?(coder: NSCoder) { nil }

    private func buildContent() {
        guard let content = window?.contentView else { return }
        let scrollView = NSScrollView()
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        textView.isEditable = false
        textView.isSelectable = true
        textView.isRichText = false
        textView.font = NSFont.monospacedSystemFont(ofSize: 11,
                                                    weight: .regular)
        textView.autoresizingMask = [.width]
        scrollView.documentView = textView

        let refreshButton = NSButton(title: "Refresh", target: self,
                                     action: #selector(refresh))
        let copyButton = NSButton(title: "Copy", target: self,
                                  action: #selector(copyReport))
        let controls = NSStackView(views: [refreshButton, copyButton])
        controls.orientation = .horizontal
        controls.spacing = 8
        controls.translatesAutoresizingMaskIntoConstraints = false

        content.addSubview(scrollView)
        content.addSubview(controls)
        NSLayoutConstraint.activate([
            controls.topAnchor.constraint(equalTo: content.topAnchor,
                                           constant: 12),
            controls.trailingAnchor.constraint(equalTo: content.trailingAnchor,
                                                constant: -12),
            scrollView.topAnchor.constraint(equalTo: controls.bottomAnchor,
                                             constant: 8),
            scrollView.leadingAnchor.constraint(equalTo: content.leadingAnchor,
                                                 constant: 12),
            scrollView.trailingAnchor.constraint(equalTo: content.trailingAnchor,
                                                  constant: -12),
            scrollView.bottomAnchor.constraint(equalTo: content.bottomAnchor,
                                                constant: -12)
        ])
    }

    @objc private func refresh() {
        _ = client.send(.updateStatus)
        textView.string = client.report(includeDebug: true)
    }

    @objc private func copyReport() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(textView.string, forType: .string)
    }
}
#endif

@MainActor
private final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let client = DriverClient()
    private let statusItem = NSStatusBar.system.statusItem(
        withLength: NSStatusItem.squareLength)
    private let menu = NSMenu()
    private var networks: [WirelessNetwork] = []
    private var connectedSSID = ""
    private var wifiEnabled = true
    private var scanRunning = false
    private var statusMessage = "Loading…"
    private var connectionPending = false
    private var signalStrength: Int?
    private var sessionCredentials: [String: (password: String,
                                               needsSave: Bool)] = [:]
    private var statusTimer: Timer?
    private var lastScanAttemptAt: Date?
    private var lastScanCompletedAt: Date?
    private var scanMenuItem: NSMenuItem?
    private var networkMenuItems: [NSMenuItem] = []
#if RTW_APP_DEBUG
    private var debugWindow: DebugWindowController?
#endif

    func applicationDidFinishLaunching(_ notification: Notification) {
        _ = notification
        menu.delegate = self
        statusItem.menu = menu
        statusItem.button?.image = WiFiStatusGlyph.image(level: 0)
        refreshStatus()
        statusTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) {
            [weak self] _ in
            Task { @MainActor in self?.refreshStatus() }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        _ = notification
        statusTimer?.invalidate()
    }

    func menuWillOpen(_ menu: NSMenu) {
        _ = menu
        refreshStatus()
        rebuildMenu()
        let scanReference = lastScanCompletedAt ?? lastScanAttemptAt
        let scanAge = scanReference.map { Date().timeIntervalSince($0) }
        if scanAge == nil || scanAge! >= 15 {
            startScan()
        }
    }

    func menuDidClose(_ menu: NSMenu) {
        _ = menu
    }

    private func refreshStatus() {
        let state = client.property("InterfaceState") ?? "Unavailable"
        let explicit = client.property("InterfaceUserEnabled")
        wifiEnabled = explicit.map { $0 == "true" } ?? (state != "Disabled")
        connectedSSID = client.property("ConnectedSSID") ?? ""
        let wifiStatus = client.property("WiFiStatus") ?? "Unavailable"
        signalStrength = client.property("SignalStrength").flatMap(Int.init)
        statusMessage = client.property("DriverStatus") ?? wifiStatus
        if let results = client.property("ScanResults") {
            networks = parseScanResults(results)
        }

        let label: String
        if !wifiEnabled {
            label = "Wi-Fi Off"
            statusItem.button?.image = NSImage(
                systemSymbolName: "wifi.slash", accessibilityDescription: label)
        } else if connectionPending || wifiStatus == "Connecting" {
            label = statusMessage
            statusItem.button?.image = WiFiStatusGlyph.image(level: 0)
        } else if wifiStatus == "Connected" && !connectedSSID.isEmpty {
            label = "Connected to \(connectedSSID)"
            connectionPending = false
            statusItem.button?.image = WiFiStatusGlyph.image(
                level: WiFiStatusGlyph.level(for: signalStrength))
        } else if wifiStatus == "Scanning" {
            label = wifiStatus
            statusItem.button?.image = WiFiStatusGlyph.image(level: 0)
        } else {
            label = "Wi-Fi Disconnected"
            connectionPending = false
            statusItem.button?.image = NSImage(
                systemSymbolName: "wifi.exclamationmark",
                accessibilityDescription: label)
        }
        statusItem.button?.image?.isTemplate = true
        statusItem.button?.toolTip = label
    }

    private func rebuildMenu() {
        scanMenuItem = nil
        networkMenuItems.removeAll(keepingCapacity: true)
        menu.removeAllItems()

        let toggle = NSMenuItem(title: "Wi-Fi", action: #selector(toggleWiFi),
                                keyEquivalent: "")
        toggle.target = self
        toggle.state = wifiEnabled ? .on : .off
        menu.addItem(toggle)

        let statusTitle: String
        if !wifiEnabled {
            statusTitle = "Wi-Fi is off"
        } else if !connectedSSID.isEmpty {
            statusTitle = "Connected: \(connectedSSID)"
        } else {
            statusTitle = statusMessage
        }
        let status = NSMenuItem(title: statusTitle, action: nil,
                                keyEquivalent: "")
        status.isEnabled = false
        menu.addItem(status)
        menu.addItem(.separator())

        if wifiEnabled {
            if networks.isEmpty {
                let empty = NSMenuItem(
                    title: scanRunning ? "Searching for networks…" :
                                         "No networks found",
                    action: nil, keyEquivalent: "")
                empty.isEnabled = false
                menu.addItem(empty)
            } else {
                for network in networks {
                    let suffix = network.isSecure ? "  —  \(network.rssi) dBm" :
                                                    "  —  Open, \(network.rssi) dBm"
                    let item = NSMenuItem(title: network.ssid + suffix,
                                          action: #selector(selectNetwork(_:)),
                                          keyEquivalent: "")
                    item.target = self
                    item.representedObject = NetworkBox(network)
                    item.state = network.ssid == connectedSSID ? .on : .off
                    item.image = NSImage(
                        systemSymbolName: network.isSecure ? "lock.fill" : "wifi",
                        accessibilityDescription: network.isSecure ?
                            "Secured network" : "Open network")
                    item.image?.isTemplate = true
                    item.isEnabled = !scanRunning
                    networkMenuItems.append(item)
                    menu.addItem(item)
                }
            }

            menu.addItem(.separator())
            let refresh = NSMenuItem(
                title: scanRunning ? "Searching…" : "Search for Networks",
                action: #selector(refreshNetworks), keyEquivalent: "r")
            refresh.target = self
            refresh.isEnabled = !scanRunning
            scanMenuItem = refresh
            menu.addItem(refresh)
            if !connectedSSID.isEmpty {
                let disconnect = NSMenuItem(title: "Disconnect",
                                            action: #selector(disconnect),
                                            keyEquivalent: "")
                disconnect.target = self
                menu.addItem(disconnect)
            }
        }

        menu.addItem(.separator())
        let launchAtLogin = NSMenuItem(title: "Launch at Login",
                                       action: #selector(toggleLaunchAtLogin),
                                       keyEquivalent: "")
        launchAtLogin.target = self
        launchAtLogin.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(launchAtLogin)

#if RTW_APP_DEBUG
        let debug = NSMenuItem(title: "Debug Info…",
                               action: #selector(showDebugInfo),
                               keyEquivalent: "d")
        debug.target = self
        menu.addItem(debug)
#endif

        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit Realtek RTL8822C Wi-Fi",
                              action: #selector(quitApplication),
                              keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
    }

    @objc private func toggleWiFi() {
        let requested = !wifiEnabled
        performAsync { [client] in
            client.send(.setInterfaceEnabled, enabled: requested)
        } completion: { [weak self] result in
            guard let self else { return }
            if result != ioSuccess {
                self.showError(title: "Unable to change Wi-Fi state",
                               result: result)
            }
            self.refreshStatus()
        }
    }

    @objc private func refreshNetworks() { startScan() }

    private func startScan() {
        guard wifiEnabled, !scanRunning else { return }
        scanRunning = true
        lastScanAttemptAt = Date()
        scanMenuItem?.title = "Searching…"
        scanMenuItem?.isEnabled = false
        for item in networkMenuItems { item.isEnabled = false }
        performAsync { [client] in
            let connected = !(client.property("ConnectedSSID") ?? "").isEmpty
            let result = client.send(.scan)
            guard result == ioSuccess else { return result }
            // Disconnected scans complete synchronously and do not update the
            // connected-scan state property, which may still contain an old
            // terminal result from an earlier association.
            if !connected { return ioSuccess }
            var observedActiveScan = false
            for _ in 0..<80 {
                let state = client.property("ConnectedScanState") ?? ""
                if state.contains("active=1") {
                    observedActiveScan = true
                    Thread.sleep(forTimeInterval: 0.2)
                    continue
                }
                if state.contains("result=cancelled") { return ioAborted }
                if state.contains("result=complete") || !observedActiveScan {
                    return ioSuccess
                }
                return ioError
            }
            return ioTimeout
        } completion: { [weak self] result in
            guard let self else { return }
            self.scanRunning = false
            if result == ioSuccess {
                self.lastScanCompletedAt = Date()
            } else {
                self.statusMessage = "Scan failed: \(ioReturnDescription(result))"
            }
            self.refreshStatus()
            // A single menu replacement at sweep completion is bounded and
            // makes an automatic scan visible. Never rebuild per beacon or
            // from the periodic status timer.
            self.rebuildMenu()
        }
    }

    @objc private func selectNetwork(_ sender: NSMenuItem) {
        guard let box = sender.representedObject as? NetworkBox else { return }
        let network = box.network
        if network.ssid == connectedSSID { return }

        var password = ""
        var needsSave = false
        if network.isSecure {
            if let cached = sessionCredentials[network.ssid] {
                password = cached.password
                needsSave = cached.needsSave
            } else if let saved = KeychainStore.password(for: network.ssid) {
                password = saved
                sessionCredentials[network.ssid] = (saved, false)
            } else if let entered = requestPassword(for: network.ssid) {
                password = entered
                needsSave = true
                sessionCredentials[network.ssid] = (entered, true)
            } else {
                return
            }
        }
        connect(to: network, password: password, needsSave: needsSave)
    }

    private func connect(to network: WirelessNetwork, password: String,
                         needsSave: Bool) {
        connectionPending = true
        statusMessage = "Connecting to \(network.ssid)…"
        statusItem.button?.image = WiFiStatusGlyph.image(level: 0)
        statusItem.button?.toolTip = statusMessage
        rebuildMenu()
        performAsync { [client] in
            let result = client.send(.connect, ssid: network.ssid,
                                     password: password)
            guard result == ioSuccess else { return result }
            for _ in 0..<120 {
                let state = client.property("WiFiStatus") ?? ""
                if state == "Connected" { return ioSuccess }
                if state == "Failed" || state.contains("Password") ||
                    state.contains("Unsupported") { return ioError }
                Thread.sleep(forTimeInterval: 0.25)
            }
            return ioTimeout
        } completion: { [weak self] result in
            guard let self else { return }
            self.connectionPending = false
            self.refreshStatus()
            if result == ioSuccess && self.connectedSSID == network.ssid {
                if network.isSecure && needsSave {
                    KeychainStore.save(password: password, for: network.ssid)
                    self.sessionCredentials[network.ssid] = (password, false)
                }
            } else {
                let detail = self.client.property("DriverStatus") ??
                             ioReturnDescription(result)
                let wifiState = self.client.property("WiFiStatus") ?? ""
                let failure = (detail + " " + wifiState).lowercased()
                let credentialFailure = failure.contains("password") ||
                    failure.contains("four-way") || failure.contains("mic") ||
                    failure.contains("handshake rejected")
                if network.isSecure && credentialFailure {
                    KeychainStore.removePassword(for: network.ssid)
                    self.sessionCredentials.removeValue(forKey: network.ssid)
                }
                self.showError(title: "Could not connect to \(network.ssid)",
                               message: detail)
            }
            self.rebuildMenu()
        }
    }

    @objc private func disconnect() {
        performAsync { [client] in client.send(.disconnect) } completion: {
            [weak self] result in
            guard let self else { return }
            if result != ioSuccess {
                self.showError(title: "Unable to disconnect", result: result)
            }
            self.refreshStatus()
        }
    }

    private func requestPassword(for ssid: String) -> String? {
        let alert = NSAlert()
        alert.messageText = "Enter the password for “\(ssid)”"
        alert.informativeText = "The password will be stored in your macOS Keychain."
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")
        let field = NSSecureTextField(frame: NSRect(x: 0, y: 0,
                                                   width: 300, height: 24))
        field.placeholderString = "WPA2 password"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        guard alert.runModal() == .alertFirstButtonReturn else { return nil }
        return field.stringValue
    }

#if RTW_APP_DEBUG
    @objc private func showDebugInfo() {
        if debugWindow == nil { debugWindow = DebugWindowController(client: client) }
        debugWindow?.showWindow(nil)
        debugWindow?.window?.makeKeyAndOrderFront(nil)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }
#endif

    @objc private func quitApplication() { NSApplication.shared.terminate(nil) }

    @objc private func toggleLaunchAtLogin() {
        do {
            if SMAppService.mainApp.status == .enabled {
                try SMAppService.mainApp.unregister()
            } else {
                try SMAppService.mainApp.register()
            }
        } catch {
            showError(title: "Unable to change Login Item",
                      message: error.localizedDescription)
        }
        rebuildMenu()
    }

    private func showError(title: String, result: Int32) {
        showError(title: title, message: ioReturnDescription(result))
    }

    private func showError(title: String, message: String) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = title
        alert.informativeText = message
        alert.runModal()
    }

    private func performAsync(_ work: @escaping @Sendable () -> Int32,
                              completion: @escaping @MainActor (Int32) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let result = work()
            DispatchQueue.main.async { completion(result) }
        }
    }
}

@main
private struct RealtekRTL8822CMenuApplication {
    @MainActor
    static func main() {
        let application = NSApplication.shared
        let delegate = AppDelegate()
        application.delegate = delegate
        application.setActivationPolicy(.accessory)
        application.run()
    }
}
