// SPDX-License-Identifier: GPL-3.0-or-later
// How a wizard sits on its sequence's window: centred on it at about 80% of
// its size, kept above it and moving with it, re-centred when it's resized,
// and the window behind faded toward black while the wizard is key. The gel
// wizard uses it; the PCR wizard still carries its own copy of the same.
import AppKit

final class WizardChrome {
    private weak var parent: NSWindow?
    private weak var window: NSWindow?
    /// Darkens the parent window while the wizard is in front.
    private var dim: NSWindow?
    private var parentResize: NSObjectProtocol?

    /// Places `window` on `parent` and keeps it there until `detach()`.
    init(window: NSWindow, parent: NSWindow?) {
        self.window = window
        self.parent = parent
        guard let parent = parent else { return }
        let f = parent.frame
        var size = NSSize(width: max(window.minSize.width, f.width * 0.8),
                          height: max(window.minSize.height, f.height * 0.8))
        if let screen = (parent.screen ?? NSScreen.main)?.visibleFrame {
            size.width = min(size.width, screen.width)
            size.height = min(size.height, screen.height)
        }
        window.setFrame(NSRect(x: f.midX - size.width / 2, y: f.midY - size.height / 2,
                               width: size.width, height: size.height), display: false)
        parentResize = NotificationCenter.default.addObserver(
            forName: NSWindow.didResizeNotification, object: parent, queue: .main) { [weak self] _ in
            self?.followParent()
        }
    }

    /// Call after the window is shown: kept above the parent, moving with it,
    /// so clicking the parent never buries the wizard behind it.
    func attach() {
        guard let parent = parent, let window = window else { return }
        parent.addChildWindow(window, ordered: .above)
    }

    /// Re-centres the wizard at 80% of the parent and moves the dark layer with it.
    private func followParent() {
        guard let parent = parent, let window = window else { return }
        dim?.setFrame(parent.frame, display: true)
        let f = parent.frame
        let size = NSSize(width: max(window.minSize.width, f.width * 0.8),
                          height: max(window.minSize.height, f.height * 0.8))
        window.setFrame(NSRect(x: f.midX - size.width / 2, y: f.midY - size.height / 2,
                               width: size.width, height: size.height), display: true)
        // The content may need more than asked for; centre what it took.
        let actual = window.frame.size
        if actual != size {
            window.setFrameOrigin(NSPoint(x: f.midX - actual.width / 2, y: f.midY - actual.height / 2))
        }
    }

    /// Fades the parent toward black while the wizard is key, so the wizard
    /// reads clearly; clicks still go through.
    func setDimmed(_ on: Bool) {
        guard let parent = parent, parent.isVisible else { return }
        if on && dim == nil {
            let overlay = NSWindow(contentRect: parent.frame, styleMask: .borderless, backing: .buffered, defer: false)
            overlay.isOpaque = false
            overlay.hasShadow = false
            overlay.ignoresMouseEvents = true
            overlay.isReleasedWhenClosed = false
            overlay.backgroundColor = NSColor.black.withAlphaComponent(0.45)
            overlay.alphaValue = 0
            parent.addChildWindow(overlay, ordered: .above)
            dim = overlay
            // The wizard goes back on top of the dark layer.
            if let window = window, window.parent === parent {
                parent.removeChildWindow(window)
                parent.addChildWindow(window, ordered: .above)
            }
        }
        guard let overlay = dim else { return }
        overlay.setFrame(parent.frame, display: false)
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.2
            overlay.animator().alphaValue = on ? 1 : 0
        }
    }

    /// Call from windowWillClose.
    func detach() {
        if let overlay = dim {
            overlay.parent?.removeChildWindow(overlay)
            overlay.orderOut(nil)
            dim = nil
        }
        if let observer = parentResize { NotificationCenter.default.removeObserver(observer) }
        parentResize = nil
        if let window = window { window.parent?.removeChildWindow(window) }
    }
}
