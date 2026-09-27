// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 RealtekRTL8822C contributors.

import AppKit
import Foundation
import Security
import ServiceManagement
import UserNotifications

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

@_silgen_name("RTWClientCopyScanSnapshotJSON")
private func RTWClientCopyScanSnapshotJSON(
    _ output: UnsafeMutablePointer<CChar>, _ capacity: Int) -> Int32

@_silgen_name("RTWClientGetAvailability")
private func RTWClientGetAvailability() -> UInt32

private enum DriverCommand: UInt32 {
    case updateStatus = 1
    case scan = 2
    case connect = 3
    case disconnect = 4
    case setInterfaceEnabled = 5
    case directedScan = 6
    case cancelConnection = 7
}

private let ioSuccess: Int32 = 0
private let ioError = Int32(bitPattern: 0xe00002bc)
private let ioTimeout = Int32(bitPattern: 0xe00002d6)
private let ioAborted = Int32(bitPattern: 0xe00002eb)
private let ioNotReady = Int32(bitPattern: 0xe00002d8)
private let projectReleasesURL = URL(string: "https://github.com/flany-dev/RealtekRTL8822C/releases")!

private enum DriverAvailability: UInt32 {
    case unsupportedHardware = 0
    case kextNotLoaded = 1
    case ready = 2
    case initializationFailed = 3
}

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

    func scanSnapshot() -> ScanSnapshot? {
        var buffer = [CChar](repeating: 0, count: 64 * 1024)
        let result = RTWClientCopyScanSnapshotJSON(&buffer, buffer.count)
        guard result == ioSuccess else { return nil }
        let length = buffer.firstIndex(of: 0) ?? buffer.count
        let data = Data(buffer[0..<length].map { UInt8(bitPattern: $0) })
        return try? JSONDecoder().decode(ScanSnapshot.self, from: data)
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

private struct ScanSnapshot: Decodable {
    let version: UInt32
    let generation: UInt32
    let entries: [ScanSnapshotEntry]
}

private struct ScanSnapshotEntry: Decodable {
    let bssid: String
    let ssidHex: String
    let hidden: Bool
    let security: UInt8
    let channel: Int
    let centerChannel: Int
    let bandwidth: Int
    let rssi: Int
    let flags: UInt8
    let ageMs: UInt32

    var ssid: String {
        var bytes: [UInt8] = []
        var index = ssidHex.startIndex
        while index < ssidHex.endIndex {
            let next = ssidHex.index(index, offsetBy: 2,
                                     limitedBy: ssidHex.endIndex) ??
                       ssidHex.endIndex
            guard next > index,
                  let byte = UInt8(ssidHex[index..<next], radix: 16) else {
                return ""
            }
            bytes.append(byte)
            index = next
        }
        return String(decoding: bytes, as: UTF8.self)
    }
}

private struct WirelessNetwork: Equatable {
    let ssid: String
    let securityName: String
    let channel: Int
    let centerChannel: Int
    let bandwidth: Int
    let rssi: Int
    let bssid: String
    let hidden: Bool
    let ageMs: UInt32
    let flags: UInt8

    var isSecure: Bool { securityName != "Open" }
    var requiresDfs: Bool {
        (flags & (1 << 7)) != 0 || (channel >= 52 && channel <= 144)
    }
    var isConnectable: Bool {
        if channel == 0 { return true }
        if (flags & (1 << 6)) != 0 { return true }
        // Compatibility with an already-running pre-capability v0.0.3 kext.
        return (channel >= 1 && channel <= 11) ||
            [36, 40, 44, 48, 149, 153, 157, 161, 165].contains(channel)
    }
    var displayName: String {
        if !hidden { return ssid }
        let suffix = bssid.split(separator: ":").suffix(2).joined(separator: ":")
        return suffix.isEmpty ? "Hidden Network" : "Hidden Network (\(suffix))"
    }
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

            let center = NSPoint(x: 9.5, y: 5.35)
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
            NSBezierPath(ovalIn: NSRect(x: 7.9, y: 3.75,
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

private func buildWirelessNetworks(from snapshot: ScanSnapshot) -> [WirelessNetwork] {
    return snapshot.entries.map { entry in
        let securityName: String
        switch entry.security {
            case 2: securityName = "WPA2"
            case 1: securityName = "WPA"
            default: securityName = "Open"
        }
        return WirelessNetwork(
            ssid: entry.ssid,
            securityName: securityName,
            channel: entry.channel,
            centerChannel: entry.centerChannel,
            bandwidth: entry.bandwidth,
            rssi: entry.rssi,
            bssid: entry.bssid,
            hidden: entry.hidden,
            ageMs: entry.ageMs,
            flags: entry.flags)
    }.sorted {
        if $0.rssi != $1.rssi { return $0.rssi > $1.rssi }
        let nameOrder = $0.displayName.localizedCaseInsensitiveCompare($1.displayName)
        if nameOrder != .orderedSame { return nameOrder == .orderedAscending }
        return $0.bssid < $1.bssid
    }
}

@MainActor
private final class ReportWindowController: NSWindowController {
    private let textView = NSTextView()
    private let client: DriverClient
    private let includeDebug: Bool
    private let refreshDriver: Bool

    init(client: DriverClient, title: String, includeDebug: Bool,
         refreshDriver: Bool) {
        self.client = client
        self.includeDebug = includeDebug
        self.refreshDriver = refreshDriver
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 820, height: 600),
            styleMask: [.titled, .closable, .resizable, .miniaturizable],
            backing: .buffered, defer: false)
        window.title = title
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
        if refreshDriver { _ = client.send(.updateStatus) }
        textView.string = client.report(includeDebug: includeDebug)
    }

    @objc private func copyReport() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(textView.string, forType: .string)
    }
}

@MainActor
private final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate,
                                 UNUserNotificationCenterDelegate {
    private let client = DriverClient()
    private let statusItem = NSStatusBar.system.statusItem(
        withLength: NSStatusItem.squareLength)
    private let menu = NSMenu()
    private var networks: [WirelessNetwork] = []
    private var scanGeneration: UInt32 = 0
    private var scanSnapshotInitialized = false
    private var scanSnapshotLoadGeneration: UInt64 = 0
    private var scanSnapshotLoadPending = false
    private var scanOperationGeneration: UInt64 = 0
    private var connectionOperationGeneration: UInt64 = 0
    private var interfaceOperationGeneration: UInt64 = 0
    private var interfaceOperationPending = false
    private var connectedSSID = ""
    private var wifiEnabled = true
    private var scanRunning = false
    private var statusMessage = "Loading…"
    private var connectionPending = false
    private var localConnectionCancellationRequested = false
    private var signalStrength: Int?
    private var sessionCredentials: [String: (password: String,
                                               needsSave: Bool)] = [:]
    private var statusTimer: Timer?
    private var lastScanAttemptAt: Date?
    private var lastScanCompletedAt: Date?
    private var scanMenuItem: NSMenuItem?
    private var networkMenuItems: [NSMenuItem] = []
    private var driverAvailability: DriverAvailability = .unsupportedHardware
    private var debugDriver = false
    private var failedDebugDriver = false
    private var linkStateInitialized = false
    private var previousWiFiStatus = ""
    private var previousConnectedSSID = ""
    private var localDisconnectRequested = false
    private var lastNotificationKey = ""
    private var lastLinkEventGeneration: UInt32?
    private var notificationAuthorizationResolved = false
    private var notificationAuthorizationGranted = false
    private var pendingNotification: (key: String, title: String, body: String)?
    private var notificationKeysInFlight: Set<String> = []
    private var notificationDeliveryError: String?
    private var debugWindow: ReportWindowController?
    private var failInfoWindow: ReportWindowController?

    func applicationDidFinishLaunching(_ notification: Notification) {
        _ = notification
        menu.delegate = self
        statusItem.menu = menu
        statusItem.button?.image = WiFiStatusGlyph.image(level: 0)
        UNUserNotificationCenter.current().delegate = self
        UNUserNotificationCenter.current().requestAuthorization(
            options: [.alert, .sound]) { [weak self] granted, _ in
            DispatchQueue.main.async {
                guard let self else { return }
                self.notificationAuthorizationResolved = true
                self.notificationAuthorizationGranted = granted
                self.notificationDeliveryError = nil
                if granted, let pending = self.pendingNotification {
                    self.pendingNotification = nil
                    self.postNotification(key: pending.key,
                                          title: pending.title,
                                          body: pending.body)
                } else if !granted {
                    self.pendingNotification = nil
                }
                self.rebuildMenu()
            }
        }
        refreshStatus()
        statusTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) {
            [weak self] _ in
            Task { @MainActor [weak self] in self?.refreshStatus() }
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
        if !connectionPending && (scanAge == nil || scanAge! >= 15) {
            startScan()
        }
    }

    func menuDidClose(_ menu: NSMenu) {
        _ = menu
    }

    private func refreshStatus() {
        driverAvailability = DriverAvailability(rawValue: RTWClientGetAvailability()) ??
            .unsupportedHardware
        let buildConfiguration: String?
        if driverAvailability == .ready {
            buildConfiguration = client.property("BuildConfiguration") ??
                client.property("RTL8822CBuildConfiguration")
        } else {
            buildConfiguration = client.property("RTL8822CBuildConfiguration") ??
                client.property("BuildConfiguration")
        }
        debugDriver = buildConfiguration?.caseInsensitiveCompare("Debug") ==
            .orderedSame
        failedDebugDriver = driverAvailability == .initializationFailed && debugDriver
        guard driverAvailability == .ready else {
            wifiEnabled = false
            connectedSSID = ""
            networks.removeAll(keepingCapacity: true)
            scanSnapshotInitialized = false
            scanSnapshotLoadGeneration &+= 1
            scanSnapshotLoadPending = false
            connectionPending = false
            switch driverAvailability {
            case .initializationFailed:
                let stage = client.property("RTL8822CStartStage") ?? "unknown stage"
                let failure = client.property("RTL8822CStartFailure") ??
                    "unspecified hardware initialization error"
                statusMessage = "RTL8822C initialization failed at \(stage): \(failure)"
            case .kextNotLoaded:
                statusMessage = "RTL8822C kext is not loaded"
            default:
                statusMessage = "RTL8822C hardware is not supported or was not detected"
            }
            statusItem.button?.image = NSImage(
                systemSymbolName: "wifi.exclamationmark",
                accessibilityDescription: statusMessage)
            statusItem.button?.image?.isTemplate = true
            statusItem.button?.toolTip = statusMessage
            return
        }
        let state = client.property("InterfaceState") ?? "Unavailable"
        let explicit = client.property("InterfaceUserEnabled")
        wifiEnabled = explicit.map { $0 == "true" } ?? (state != "Disabled")
        connectedSSID = client.property("ConnectedSSID") ?? ""
        let wifiStatus = client.property("WiFiStatus") ?? "Unavailable"
        let connectionResult = client.property("ConnectionResult") ?? "none"
        let connectionPhase = client.property("ConnectionPhase") ?? "idle"
        if connectionResult == "pending" { connectionPending = true }
        signalStrength = client.property("SignalStrength").flatMap(Int.init)
        statusMessage = connectionPending ?
            connectionPhaseDescription(connectionPhase) :
            (client.property("DriverStatus") ?? wifiStatus)
        if let generationText = client.property("LinkEventGeneration"),
           let generation = UInt32(generationText) {
            handleLinkEvent(generation: generation,
                            type: client.property("LinkEventType") ?? "unknown",
                            ssid: client.property("LinkEventSSID") ?? "",
                            reason: client.property("LinkEventReason") ?? "unknown")
        } else {
            if lastLinkEventGeneration == nil { lastLinkEventGeneration = 0 }
            handleLinkTransition(wifiStatus: wifiStatus, ssid: connectedSSID)
        }
        let publishedScanGeneration = client.property("ScanGeneration")
            .flatMap(UInt32.init) ?? scanGeneration
        refreshScanSnapshotIfNeeded(publishedGeneration: publishedScanGeneration)

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
            statusItem.button?.image = WiFiStatusGlyph.image(
                level: WiFiStatusGlyph.level(for: signalStrength))
        } else if wifiStatus == "Scanning" {
            label = wifiStatus
            statusItem.button?.image = WiFiStatusGlyph.image(level: 0)
        } else {
            label = "Wi-Fi Disconnected"
            statusItem.button?.image = NSImage(
                systemSymbolName: "wifi.exclamationmark",
                accessibilityDescription: label)
        }
        statusItem.button?.image?.isTemplate = true
        statusItem.button?.toolTip = label
    }

    private func refreshScanSnapshotIfNeeded(publishedGeneration: UInt32) {
        guard (!scanSnapshotInitialized || publishedGeneration != scanGeneration),
              !scanSnapshotLoadPending else { return }
        scanSnapshotLoadGeneration &+= 1
        let loadGeneration = scanSnapshotLoadGeneration
        scanSnapshotLoadPending = true
        DispatchQueue.global(qos: .userInitiated).async { [client] in
            let snapshot = client.scanSnapshot()
            DispatchQueue.main.async { [weak self] in
                guard let self,
                      loadGeneration == self.scanSnapshotLoadGeneration else {
                    return
                }
                self.scanSnapshotLoadPending = false
                guard let snapshot else { return }
                // A newer snapshot is always safe; an older asynchronous read
                // must never roll the menu model backwards.
                guard !self.scanSnapshotInitialized ||
                      snapshot.generation >= self.scanGeneration else { return }
                self.networks = buildWirelessNetworks(from: snapshot)
                self.scanGeneration = snapshot.generation
                self.scanSnapshotInitialized = true
                self.rebuildMenu()
            }
        }
    }

    private func rebuildMenu() {
        scanMenuItem = nil
        networkMenuItems.removeAll(keepingCapacity: true)
        menu.removeAllItems()

        if driverAvailability != .ready {
            let status = NSMenuItem(title: statusMessage, action: nil,
                                    keyEquivalent: "")
            status.isEnabled = false
            menu.addItem(status)
            let detailText: String
            switch driverAvailability {
            case .initializationFailed:
                detailText = "Compatibility details are available in the driver report."
            case .kextNotLoaded:
                detailText = "Compatible RTL8822CE hardware was detected."
            default:
                detailText = "A supported Realtek RTL8822CE (10ec:c822) was not found."
            }
            let detail = NSMenuItem(title: detailText, action: nil,
                                    keyEquivalent: "")
            detail.isEnabled = false
            menu.addItem(detail)
            if driverAvailability == .initializationFailed && failedDebugDriver {
                let failInfo = NSMenuItem(
                    title: "Fail Info…", action: #selector(showFailInfo),
                    keyEquivalent: "")
                failInfo.target = self
                menu.addItem(failInfo)
            } else if driverAvailability == .initializationFailed {
                let report = NSMenuItem(
                    title: "Copy Compatibility Report",
                    action: #selector(copyCompatibilityReport),
                    keyEquivalent: "")
                report.target = self
                menu.addItem(report)
            }
            menu.addItem(.separator())
            let download = NSMenuItem(title: "Download RealtekRTL8822C…",
                                      action: #selector(openProjectReleases),
                                      keyEquivalent: "")
            download.target = self
            menu.addItem(download)
            menu.addItem(.separator())
            let quit = NSMenuItem(title: "Quit Realtek RTL8822C Wi-Fi",
                                  action: #selector(quitApplication),
                                  keyEquivalent: "q")
            quit.target = self
            menu.addItem(quit)
            return
        }

        let toggle = NSMenuItem(title: "Wi-Fi", action: #selector(toggleWiFi),
                                keyEquivalent: "")
        toggle.target = self
        toggle.state = wifiEnabled ? .on : .off
        toggle.isEnabled = !interfaceOperationPending
        menu.addItem(toggle)

        let statusTitle: String
        if !wifiEnabled {
            statusTitle = "Wi-Fi is off"
        } else if connectionPending {
            statusTitle = statusMessage
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
                    let band = network.channel > 14 ? "5 GHz" : "2.4 GHz"
                    let security = network.isSecure ? "" : "Open, "
                    let availability = network.requiresDfs ? ", DFS unavailable" :
                        (!network.isConnectable ? ", receive only" : "")
                    let suffix = "  —  \(security)\(band), ch \(network.channel)\(availability), \(network.rssi) dBm"
                    let title = network.displayName + suffix
                    let item = NSMenuItem(title: title,
                                          action: #selector(selectNetwork(_:)),
                                          keyEquivalent: "")
                    item.target = self
                    item.representedObject = NetworkBox(network)
                    item.state = (!network.hidden && !connectedSSID.isEmpty &&
                                  network.ssid == connectedSSID) ? .on : .off
                    item.image = NSImage(
                        systemSymbolName: network.isSecure ? "lock.fill" : "wifi",
                        accessibilityDescription: network.isSecure ?
                            "Secured network" : "Open network")
                    item.image?.isTemplate = true
                    item.isEnabled = !connectionPending && !network.hidden &&
                        network.isConnectable
                    let permanentlyUnavailable = network.hidden ||
                        !network.isConnectable
                    if permanentlyUnavailable {
                        // AppKit may redraw a disabled menu item with its
                        // ordinary title color after the menu is reopened.
                        // Keep receive-only/DFS/hidden rows visibly disabled
                        // across every menu reconstruction.
                        item.attributedTitle = NSAttributedString(
                            string: title,
                            attributes: [
                                .foregroundColor: NSColor.disabledControlTextColor
                            ])
                    }
                    if connectionPending && !network.hidden {
                        item.toolTip = "Cancel the current connection attempt before choosing another network."
                    }
                    if network.hidden {
                        item.toolTip = "Use Join Other Network… and enter this network's SSID."
                    } else if network.requiresDfs {
                        item.toolTip = "This network is visible, but DFS connection requires CAC and radar handling that is not implemented yet."
                    } else if !network.isConnectable {
                        item.toolTip = "This channel is receive-only under the current regulatory policy."
                    }
                    networkMenuItems.append(item)
                    menu.addItem(item)
                }
            }

            menu.addItem(.separator())
            let refresh = NSMenuItem(
                title: scanRunning ? "Searching…" : "Search for Networks",
                action: #selector(refreshNetworks), keyEquivalent: "r")
            refresh.target = self
            refresh.isEnabled = !scanRunning && !connectionPending
            scanMenuItem = refresh
            menu.addItem(refresh)
            let joinOther = NSMenuItem(title: "Join Other Network…",
                                       action: #selector(joinOtherNetwork),
                                       keyEquivalent: "")
            joinOther.target = self
            // An unknown hidden SSID needs its own directed scan. Visible BSS
            // rows can be queued safely while an ordinary sweep is active.
            joinOther.isEnabled = !scanRunning && !connectionPending
            menu.addItem(joinOther)
            let passwordCandidates = credentialCandidateSSIDs()
            if !passwordCandidates.isEmpty {
                let managePassword = NSMenuItem(
                    title: "Manage WPA2 Password…",
                    action: #selector(managePassword), keyEquivalent: "")
                managePassword.target = self
                managePassword.isEnabled = !connectionPending
                menu.addItem(managePassword)
            }
            if connectionPending {
                let cancel = NSMenuItem(title: "Cancel Connection",
                                        action: #selector(cancelConnection),
                                        keyEquivalent: "")
                cancel.target = self
                menu.addItem(cancel)
            }
            if !connectedSSID.isEmpty && !connectionPending {
                let disconnect = NSMenuItem(title: "Disconnect",
                                            action: #selector(disconnect),
                                            keyEquivalent: "")
                disconnect.target = self
                menu.addItem(disconnect)
            }
        }

        menu.addItem(.separator())
        if notificationAuthorizationResolved &&
           (!notificationAuthorizationGranted || notificationDeliveryError != nil) {
            let notificationStatus = NSMenuItem(
                title: notificationDeliveryError == nil ?
                    "Enable Notifications…" : "Notification Delivery Failed…",
                action: #selector(openNotificationSettings), keyEquivalent: "")
            notificationStatus.target = self
            notificationStatus.toolTip = notificationDeliveryError ??
                "Enable banners for Realtek RTL8822C Wi-Fi in System Settings."
            menu.addItem(notificationStatus)
        }
        let launchAtLogin = NSMenuItem(title: "Launch at Login",
                                       action: #selector(toggleLaunchAtLogin),
                                       keyEquivalent: "")
        launchAtLogin.target = self
        if #available(macOS 13.0, *) {
            launchAtLogin.state = SMAppService.mainApp.status == .enabled ? .on : .off
        } else {
            launchAtLogin.isEnabled = false
            launchAtLogin.toolTip = "Launch at Login requires macOS 13 or newer"
        }
        menu.addItem(launchAtLogin)

        if driverAvailability == .ready && debugDriver {
            let debug = NSMenuItem(title: "Debug Info…",
                                   action: #selector(showDebugInfo),
                                   keyEquivalent: "d")
            debug.target = self
            menu.addItem(debug)
        }

        menu.addItem(.separator())
        let quit = NSMenuItem(title: "Quit Realtek RTL8822C Wi-Fi",
                              action: #selector(quitApplication),
                              keyEquivalent: "q")
        quit.target = self
        menu.addItem(quit)
    }

    @objc private func toggleWiFi() {
        guard !interfaceOperationPending else { return }
        let requested = !wifiEnabled
        if !requested { localDisconnectRequested = true }
        interfaceOperationGeneration &+= 1
        let operationGeneration = interfaceOperationGeneration
        interfaceOperationPending = true
        if !requested {
            // Interface disable cancels an in-flight scan in the driver. Make
            // its older userspace completion unable to repaint the menu.
            scanOperationGeneration &+= 1
            scanRunning = false
        }
        rebuildMenu()
        performAsync { [client] in
            client.send(.setInterfaceEnabled, enabled: requested)
        } completion: { [weak self] result in
            guard let self else { return }
            guard operationGeneration == self.interfaceOperationGeneration else {
                return
            }
            self.interfaceOperationPending = false
            if result != ioSuccess {
                self.showError(title: "Unable to change Wi-Fi state",
                               result: result)
            }
            self.refreshStatus()
            self.rebuildMenu()
        }
    }

    @objc private func refreshNetworks() { startScan() }

    @objc private func joinOtherNetwork() {
        let alert = NSAlert()
        alert.messageText = "Join Other Network"
        alert.informativeText = "Enter the SSID exactly as configured. Leave the password empty for an open network."
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")
        let container = NSView(frame: NSRect(x: 0, y: 0, width: 340, height: 92))
        let ssidLabel = NSTextField(labelWithString: "Network name (SSID)")
        ssidLabel.frame = NSRect(x: 0, y: 68, width: 340, height: 18)
        let ssidField = NSTextField(frame: NSRect(x: 0, y: 42, width: 340, height: 24))
        ssidField.placeholderString = "Network name (SSID)"
        let passwordLabel = NSTextField(labelWithString: "WPA2 password (optional)")
        passwordLabel.frame = NSRect(x: 0, y: 22, width: 340, height: 18)
        let passwordField = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 340, height: 24))
        passwordField.placeholderString = "WPA2 password (optional)"
        container.addSubview(ssidLabel)
        container.addSubview(ssidField)
        container.addSubview(passwordLabel)
        container.addSubview(passwordField)
        alert.accessoryView = container
        alert.window.initialFirstResponder = ssidField
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let ssid = ssidField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !ssid.isEmpty, ssid.utf8.count <= 32 else {
            showError(title: "Invalid network name",
                      message: "The SSID must contain 1–32 UTF-8 bytes.")
            return
        }
        let password = passwordField.stringValue
        if !password.isEmpty && (password.utf8.count < 8 || password.utf8.count > 63) {
            showError(title: "Invalid password",
                      message: "A WPA2 password must contain 8–63 UTF-8 bytes.")
            return
        }
        let network = WirelessNetwork(ssid: ssid,
                                      securityName: password.isEmpty ? "Open" : "WPA2",
                                      channel: 0, centerChannel: 0, bandwidth: 20,
                                      rssi: -127, bssid: "", hidden: true,
                                      ageMs: 0, flags: 0)
        var needsSave = false
        if !password.isEmpty {
            sessionCredentials[ssid] = (password, true)
            needsSave = true
        }
        connect(to: network, password: password, needsSave: needsSave)
    }

    private func startScan() {
        guard wifiEnabled, !scanRunning, !connectionPending else { return }
        scanOperationGeneration &+= 1
        let operationGeneration = scanOperationGeneration
        scanRunning = true
        lastScanAttemptAt = Date()
        scanMenuItem?.title = "Searching…"
        scanMenuItem?.isEnabled = false
        performAsync { [client] in
            let result = client.send(.scan)
            guard result == ioSuccess else { return result }
            var observedActiveScan = false
            for _ in 0..<80 {
                let state = client.property("ScanState") ?? ""
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
            guard operationGeneration == self.scanOperationGeneration else {
                return
            }
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
        guard !connectionPending else { return }
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

    private func credentialCandidateSSIDs() -> [String] {
        var candidates = Set(networks.compactMap { network in
            network.isSecure && !network.hidden && !network.ssid.isEmpty ?
                network.ssid : nil
        })
        candidates.formUnion(sessionCredentials.keys)
        return candidates.sorted {
            $0.localizedCaseInsensitiveCompare($1) == .orderedAscending
        }
    }

    @objc private func managePassword() {
        guard !connectionPending else { return }
        let candidates = credentialCandidateSSIDs()
        guard !candidates.isEmpty else { return }

        let alert = NSAlert()
        alert.messageText = "Manage WPA2 Password"
        alert.informativeText = "Choose a network. A replacement password is saved only after a successful WPA2 connection."
        alert.addButton(withTitle: "Enter New Password")
        alert.addButton(withTitle: "Forget Password")
        alert.addButton(withTitle: "Cancel")
        let picker = NSPopUpButton(frame: NSRect(x: 0, y: 0,
                                                 width: 320, height: 26),
                                   pullsDown: false)
        picker.addItems(withTitles: candidates)
        if let connectedIndex = candidates.firstIndex(of: connectedSSID) {
            picker.selectItem(at: connectedIndex)
        }
        alert.accessoryView = picker

        let response = alert.runModal()
        guard response != .alertThirdButtonReturn,
              let ssid = picker.selectedItem?.title else { return }
        if response == .alertSecondButtonReturn {
            KeychainStore.removePassword(for: ssid)
            sessionCredentials.removeValue(forKey: ssid)
            statusMessage = "Forgot the saved password for \(ssid)"
            rebuildMenu()
            return
        }

        guard let replacement = requestPassword(for: ssid) else { return }
        guard replacement.utf8.count >= 8 && replacement.utf8.count <= 63 else {
            showError(title: "Invalid password",
                      message: "A WPA2 password must contain 8–63 UTF-8 bytes.")
            return
        }
        sessionCredentials[ssid] = (replacement, true)
        statusMessage = "The new password for \(ssid) will be saved after a successful connection"

        // Test the replacement immediately when another visible BSS is
        // selected. For the currently connected SSID, keep the new value only
        // in memory until the next real reconnect; an idempotent Connect would
        // not validate it and therefore must not persist it.
        if connectedSSID != ssid,
           let network = networks.filter({
               !$0.hidden && $0.isSecure && $0.isConnectable && $0.ssid == ssid
           }).max(by: { $0.rssi < $1.rssi }) {
            connect(to: network, password: replacement, needsSave: true)
        } else {
            rebuildMenu()
        }
    }

    private func connect(to network: WirelessNetwork, password: String,
                         needsSave: Bool) {
        guard !connectionPending else { return }
        connectionOperationGeneration &+= 1
        let operationGeneration = connectionOperationGeneration
        localConnectionCancellationRequested = false
        connectionPending = true
        statusMessage = "Connecting to \(network.ssid)…"
        statusItem.button?.image = WiFiStatusGlyph.image(level: 0)
        statusItem.button?.toolTip = statusMessage
        rebuildMenu()
        let previousAttemptID = client.property("ConnectionAttemptID")
        performAsync { [client] in
            var result = client.send(.connect, ssid: network.ssid,
                                     password: password)
            if result == ioNotReady {
                result = client.send(.directedScan, ssid: network.ssid)
                guard result == ioSuccess else { return result }
                var observedActiveScan = false
                var scanCompleted = false
                for _ in 0..<100 {
                    let state = client.property("ScanState") ?? ""
                    if state.contains("active=1") {
                        observedActiveScan = true
                        Thread.sleep(forTimeInterval: 0.1)
                        continue
                    }
                    if state.contains("result=cancelled") { return ioAborted }
                    if state.contains("result=complete") &&
                        (observedActiveScan || state.contains("directed=1")) {
                        scanCompleted = true
                        break
                    }
                    Thread.sleep(forTimeInterval: 0.1)
                }
                guard scanCompleted else { return ioTimeout }
                result = client.send(.connect, ssid: network.ssid,
                                     password: password)
            }
            guard result == ioSuccess else { return result }
            let attemptID = client.property("ConnectionAttemptID")
            for _ in 0..<120 {
                let currentAttemptID = client.property("ConnectionAttemptID")
                if attemptID != nil && currentAttemptID == attemptID {
                    let structuredResult = client.property("ConnectionResult") ?? ""
                    if structuredResult == "success" { return ioSuccess }
                    if structuredResult == "cancelled" { return ioAborted }
                    if structuredResult == "failure" { return ioError }
                }
                let state = client.property("WiFiStatus") ?? ""
                let activeSsid = client.property("ConnectedSSID") ?? ""
                if state == "Connected" && activeSsid == network.ssid {
                    return ioSuccess
                }
                if state == "Cancelled" { return ioAborted }
                if state == "Failed" || state.contains("Password") ||
                    state.contains("Unsupported") { return ioError }
                Thread.sleep(forTimeInterval: 0.25)
            }
            return ioTimeout
        } completion: { [weak self] result in
            guard let self else { return }
            guard operationGeneration == self.connectionOperationGeneration else {
                return
            }
            self.connectionPending = false
            self.refreshStatus()
            if self.localConnectionCancellationRequested || result == ioAborted {
                self.localConnectionCancellationRequested = false
                self.statusMessage = "Connection cancelled"
                self.rebuildMenu()
                return
            }
            if result == ioSuccess && self.connectedSSID == network.ssid {
                if network.isSecure && needsSave {
                    KeychainStore.save(password: password, for: network.ssid)
                    self.sessionCredentials[network.ssid] = (password, false)
                }
            } else {
                let failureCode = self.client.property(
                    "ConnectionFailureCode") ?? "unknown"
                let detail = self.connectionFailureDescription(
                    failureCode,
                    fallback: self.client.property("DriverStatus") ??
                              ioReturnDescription(result))
                let failedAttemptID = self.client.property("ConnectionAttemptID")
                let credentialFailure = failedAttemptID != previousAttemptID &&
                    failureCode == "invalid-credentials"
                let passwordRetrySuggested = credentialFailure ||
                    (failedAttemptID != previousAttemptID &&
                     failureCode == "wpa-timeout")
                if network.isSecure && passwordRetrySuggested {
                    if credentialFailure {
                        KeychainStore.removePassword(for: network.ssid)
                        self.sessionCredentials.removeValue(forKey: network.ssid)
                    }
                    if let replacement = self.requestPassword(for: network.ssid) {
                        self.sessionCredentials[network.ssid] = (replacement, true)
                        self.connect(to: network, password: replacement,
                                     needsSave: true)
                        return
                    }
                }
                self.statusMessage = "Could not connect to \(network.ssid): \(detail)"
                self.postNotification(
                    key: "connect-failed:\(network.ssid):\(detail)",
                    title: "Wi-Fi connection failed", body: detail)
            }
            self.rebuildMenu()
        }
    }

    private func connectionPhaseDescription(_ phase: String) -> String {
        switch phase {
        case "queued": return "Waiting for the network scan to finish…"
        case "preparing": return "Preparing the Wi-Fi connection…"
        case "authenticating": return "Authenticating with the network…"
        case "associating": return "Joining the network…"
        case "negotiating-wpa": return "Securing the WPA2 connection…"
        default: return "Connecting…"
        }
    }

    private func connectionFailureDescription(_ code: String,
                                              fallback: String) -> String {
        switch code {
        case "invalid-credentials": return "The WPA2 password was rejected."
        case "auth-timeout": return "The access point did not answer authentication."
        case "assoc-timeout": return "The access point did not complete association."
        case "wpa-timeout": return "The WPA2 handshake timed out."
        case "unsupported-security": return "This network uses an unsupported security mode."
        case "channel-blocked": return "Transmission on this channel is not authorized by the current regulatory policy."
        case "dfs-unavailable": return "This DFS channel requires CAC and radar handling that is not implemented yet."
        case "channel-program-failed": return "The wireless channel could not be configured."
        case "tx-power-invalid": return "The validated transmit-power profile is unavailable."
        case "ap-disconnected": return "The access point ended the connection attempt."
        case "bandwidth-mismatch": return "The access point changed the negotiated channel width."
        case "firmware-link-failed": return "Firmware link setup failed."
        case "key-install-failed": return "WPA2 key installation failed."
        case "cancelled", "replaced": return "The connection attempt was cancelled."
        default: return fallback
        }
    }

    @objc private func cancelConnection() {
        guard connectionPending else { return }
        localConnectionCancellationRequested = true
        statusMessage = "Cancelling connection…"
        rebuildMenu()
        performAsync { [client] in client.send(.cancelConnection) } completion: {
            [weak self] result in
            guard let self else { return }
            if result != ioSuccess {
                self.localConnectionCancellationRequested = false
                self.showError(title: "Unable to cancel connection", result: result)
            }
            self.refreshStatus()
        }
    }

    @objc private func disconnect() {
        localDisconnectRequested = true
        performAsync { [client] in client.send(.disconnect) } completion: {
            [weak self] result in
            guard let self else { return }
            if result != ioSuccess {
                self.showError(title: "Unable to disconnect", result: result)
            }
            self.refreshStatus()
        }
    }

    @objc private func copyCompatibilityReport() {
        let report = client.report(includeDebug: false)
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(report, forType: .string)
    }

    @objc private func showFailInfo() {
        guard driverAvailability == .initializationFailed,
              failedDebugDriver else { return }
        if failInfoWindow == nil {
            failInfoWindow = ReportWindowController(
                client: client,
                title: "Realtek RTL8822C Fail Info",
                includeDebug: true,
                refreshDriver: false)
        }
        failInfoWindow?.showWindow(nil)
        failInfoWindow?.window?.makeKeyAndOrderFront(nil)
        NSApplication.shared.activate(ignoringOtherApps: true)
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

    @objc private func showDebugInfo() {
        guard driverAvailability == .ready, debugDriver else { return }
        if debugWindow == nil {
            debugWindow = ReportWindowController(
                client: client,
                title: "Realtek RTL8822C Debug Info",
                includeDebug: true,
                refreshDriver: true)
        }
        debugWindow?.showWindow(nil)
        debugWindow?.window?.makeKeyAndOrderFront(nil)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }

    @objc private func quitApplication() { NSApplication.shared.terminate(nil) }

    @objc private func openProjectReleases() {
        NSWorkspace.shared.open(projectReleasesURL)
    }

    private func handleLinkTransition(wifiStatus: String, ssid: String) {
        guard linkStateInitialized else {
            linkStateInitialized = true
            previousWiFiStatus = wifiStatus
            previousConnectedSSID = ssid
            return
        }
        let wasConnected = previousWiFiStatus == "Connected" &&
                           !previousConnectedSSID.isEmpty
        let isConnected = wifiStatus == "Connected" && !ssid.isEmpty
        if isConnected && (!wasConnected || previousConnectedSSID != ssid) {
            postNotification(key: "connected:\(ssid)", title: "Wi-Fi connected",
                             body: "Connected to \(ssid).")
        } else if wasConnected && !isConnected {
            if !localDisconnectRequested {
                postNotification(key: "disconnected:\(previousConnectedSSID)",
                                 title: "Wi-Fi disconnected",
                                 body: "The connection to \(previousConnectedSSID) was lost.")
            }
            localDisconnectRequested = false
        }
        previousWiFiStatus = wifiStatus
        previousConnectedSSID = ssid
    }

    private func handleLinkEvent(generation: UInt32, type: String,
                                 ssid: String, reason: String) {
        guard let previousGeneration = lastLinkEventGeneration else {
            lastLinkEventGeneration = generation
            previousWiFiStatus = type == "connected" ? "Connected" : "Idle"
            previousConnectedSSID = type == "connected" ? ssid : ""
            return
        }
        guard generation != previousGeneration else { return }
        lastLinkEventGeneration = generation

        switch type {
        case "connected":
            localDisconnectRequested = false
            postNotification(key: "link:\(generation):connected",
                             title: "Wi-Fi connected",
                             body: "Connected to \(ssid).")
            previousWiFiStatus = "Connected"
            previousConnectedSSID = ssid
        case "disconnected":
            postNotification(key: "link:\(generation):disconnected",
                             title: "Wi-Fi disconnected",
                             body: ssid.isEmpty ?
                                "The Wi-Fi connection was lost." :
                                "The connection to \(ssid) was lost.")
            previousWiFiStatus = "Idle"
            previousConnectedSSID = ""
            localDisconnectRequested = false
        case "local-disconnect":
            previousWiFiStatus = "Idle"
            previousConnectedSSID = ""
            localDisconnectRequested = false
        default:
            _ = reason
        }
    }

    private func postNotification(key: String, title: String, body: String) {
        guard key != lastNotificationKey,
              !notificationKeysInFlight.contains(key) else { return }
        guard notificationAuthorizationResolved else {
            pendingNotification = (key, title, body)
            return
        }
        guard notificationAuthorizationGranted else { return }
        notificationKeysInFlight.insert(key)
        let content = UNMutableNotificationContent()
        content.title = title
        content.body = body
        content.sound = .default
        let request = UNNotificationRequest(
            identifier: "org.realtekrtl8822c.\(UUID().uuidString)",
            content: content, trigger: nil)
        UNUserNotificationCenter.current().add(request) { [weak self] error in
            DispatchQueue.main.async {
                guard let self else { return }
                self.notificationKeysInFlight.remove(key)
                if error == nil {
                    self.lastNotificationKey = key
                    self.notificationDeliveryError = nil
                } else {
                    self.notificationDeliveryError = error?.localizedDescription
                    self.rebuildMenu()
                }
            }
        }
    }

    @objc private func openNotificationSettings() {
        let address: String
        if #available(macOS 13.0, *) {
            address = "x-apple.systempreferences:com.apple.Notifications-Settings.extension"
        } else {
            address = "x-apple.systempreferences:com.apple.preference.notifications"
        }
        if let url = URL(string: address) { NSWorkspace.shared.open(url) }
    }

    nonisolated func userNotificationCenter(
        _ center: UNUserNotificationCenter,
        willPresent notification: UNNotification,
        withCompletionHandler completionHandler:
            @escaping (UNNotificationPresentationOptions) -> Void) {
        _ = center
        _ = notification
        completionHandler([.banner, .sound])
    }

    @objc private func toggleLaunchAtLogin() {
        guard #available(macOS 13.0, *) else { return }
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
