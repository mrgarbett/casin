// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import RestrictoCore

/// A small label that follows the mouse while bases are being selected in the
/// map or the sequence: the positions, the length and, for primer-sized
/// stretches, the melting temperature. It fades shortly after the mouse is up.
final class SelectionBadge {
    static let shared = SelectionBadge()

    private let panel: NSPanel
    private let label = NSTextField(labelWithString: "")
    private var hideWork: DispatchWorkItem?
    /// Longer selections get no Tm: nearest-neighbor Tm is for oligos.
    private static let maxTmLength = 100

    private init() {
        panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 160, height: 24),
                        styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: true)
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = true
        panel.ignoresMouseEvents = true
        panel.level = .floating
        panel.hidesOnDeactivate = true
        let box = NSVisualEffectView()
        box.material = .toolTip
        box.state = .active
        box.wantsLayer = true
        box.layer?.cornerRadius = 6
        box.layer?.masksToBounds = true
        label.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .medium)
        label.textColor = .labelColor
        label.translatesAutoresizingMaskIntoConstraints = false
        box.addSubview(label)
        NSLayoutConstraint.activate([
            label.leadingAnchor.constraint(equalTo: box.leadingAnchor, constant: 8),
            label.trailingAnchor.constraint(equalTo: box.trailingAnchor, constant: -8),
            label.centerYAnchor.constraint(equalTo: box.centerYAnchor),
        ])
        panel.contentView = box
    }

    /// The text for the document's current selection, or nil when nothing is selected.
    static func text(for document: CoreDocument) -> String? {
        guard document.hasSelection, document.selectionLength > 0 else { return nil }
        let n = document.length, length = document.selectionLength
        let start = document.selectionStart + 1
        var end = document.selectionStart + length
        if end > n { end -= n }
        var parts = ["\(start.formatted())–\(end.formatted())", "\(length.formatted()) bp"]
        if length <= maxTmLength {
            var check = OCPrimerCheck()
            _ = takeString(oc_primer_check(document.selectedBases, &check))
            if !check.tm.isNaN { parts.append(String(format: "Tm %.1f °C", check.tm)) }
        }
        return parts.joined(separator: "  ·  ")
    }

    /// Shows the badge for `document`'s selection just above the mouse in `view`.
    func show(for document: CoreDocument, event: NSEvent, in view: NSView) {
        guard let window = view.window, let text = Self.text(for: document) else { hide(); return }
        hideWork?.cancel()
        label.stringValue = text
        let size = NSSize(width: ceil(label.intrinsicContentSize.width) + 16, height: 24)
        let mouse = window.convertPoint(toScreen: event.locationInWindow)
        panel.setFrame(NSRect(x: mouse.x + 12, y: mouse.y + 14, width: size.width, height: size.height),
                       display: true)
        if panel.parent !== window {
            panel.parent?.removeChildWindow(panel)
            window.addChildWindow(panel, ordered: .above)
        }
        panel.alphaValue = 1
        panel.orderFront(nil)
    }

    /// Fades the badge out after a moment, so the numbers can still be read.
    func hideSoon() {
        hideWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.hide() }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: work)
    }

    func hide() {
        hideWork?.cancel()
        panel.parent?.removeChildWindow(panel)
        panel.orderOut(nil)
    }
}
