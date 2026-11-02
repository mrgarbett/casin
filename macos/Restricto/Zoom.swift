// SPDX-License-Identifier: GPL-3.0-or-later
// Zoom for the map and the sequence, like a web page's: ⌘+ makes the text
// bigger in both and the sequence rewraps; ⌘0 goes back to 100%. One level
// for every window, kept between launches.
import AppKit

final class Zoom: NSObject, NSMenuItemValidation {
    static let shared = Zoom()
    static let changed = Notification.Name("RestrictoZoomChanged")
    private static let key = "Zoom"
    private static let steps: [Double] = [0.8, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2]

    /// The zoom now, 0.8 to 2.
    static var level: Double {
        get {
            let saved = UserDefaults.standard.double(forKey: key)
            return saved == 0 ? 1 : min(steps.last!, max(steps.first!, saved))
        }
        set {
            UserDefaults.standard.set(newValue, forKey: key)
            NotificationCenter.default.post(name: changed, object: nil)
        }
    }

    @objc func zoomIn(_ sender: Any?) {
        guard let next = Self.steps.first(where: { $0 > Self.level + 0.001 }) else { NSSound.beep(); return }
        Self.level = next
    }

    @objc func zoomOut(_ sender: Any?) {
        guard let next = Self.steps.last(where: { $0 < Self.level - 0.001 }) else { NSSound.beep(); return }
        Self.level = next
    }

    @objc func actualSize(_ sender: Any?) {
        if Self.level != 1 { Self.level = 1 }
    }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        switch menuItem.action {
        case #selector(zoomIn(_:)): return Self.level < Self.steps.last! - 0.001
        case #selector(zoomOut(_:)): return Self.level > Self.steps.first! + 0.001
        case #selector(actualSize(_:)):
            // The level now, where it's easy to find.
            menuItem.title = Self.level == 1 ? "Actual Size" : "Actual Size (now \(Int((Self.level * 100).rounded()))%)"
            return Self.level != 1
        default: return true
        }
    }

    /// View menu items: Zoom In (⌘+, and ⌘= without shift), Zoom Out, Actual Size.
    static func menuItems() -> [NSMenuItem] {
        func item(_ title: String, _ action: Selector, _ key: String) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
            i.keyEquivalentModifierMask = .command
            i.target = shared
            return i
        }
        let equals = item("Zoom In", #selector(zoomIn(_:)), "=")
        equals.isHidden = true
        equals.allowsKeyEquivalentWhenHidden = true
        return [item("Zoom In", #selector(zoomIn(_:)), "+"), equals,
                item("Zoom Out", #selector(zoomOut(_:)), "-"),
                item("Actual Size", #selector(actualSize(_:)), "0")]
    }
}
