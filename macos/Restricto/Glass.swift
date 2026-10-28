// SPDX-License-Identifier: GPL-3.0-or-later
// Liquid Glass on macOS 26 and later; plain AppKit on macOS 13 to 15.
//
// The glass classes exist only in the macOS 26 SDK (Xcode 26, Swift 6.2), so
// every use sits behind a compile-time check, which lets older Xcodes still
// build the app, and a run-time #available check, which lets the same binary
// still run on Ventura. Everything else in the app is the same on both.
import AppKit

enum Glass {
    /// True when the running system draws Liquid Glass.
    static var isAvailable: Bool {
        #if compiler(>=6.2)
        if #available(macOS 26, *) { return true }
        #endif
        return false
    }

    /// Holds glass capsules that sit side by side so they blend into one
    /// shape as they come within `spacing` of each other (the status pill
    /// splitting into two when bases are selected). Nil before macOS 26.
    static func mergingContainer(around content: NSView, spacing: CGFloat) -> NSView? {
        #if compiler(>=6.2)
        if #available(macOS 26, *) {
            let container = NSGlassEffectContainerView()
            container.contentView = content
            container.spacing = spacing
            container.translatesAutoresizingMaskIntoConstraints = false
            content.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                content.leadingAnchor.constraint(equalTo: container.leadingAnchor),
                content.trailingAnchor.constraint(equalTo: container.trailingAnchor),
                content.topAnchor.constraint(equalTo: container.topAnchor),
                content.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            ])
            return container
        }
        #endif
        return nil
    }

    /// The backdrop of a side panel (the folder sidebar, the right panel): the
    /// desktop behind the window shows through, blurred, on every version, so
    /// the panels read as translucent rather than echoing the middle.
    static func sidePanelBackground(frame: NSRect) -> NSView {
        let view = NSVisualEffectView(frame: frame)
        view.material = .sidebar
        view.blendingMode = .behindWindow
        view.state = .followsWindowActiveState
        return view
    }

    /// A glass capsule around `content`, for a control that floats over the
    /// document. Nil before macOS 26, where callers keep a plain layout.
    static func capsule(around content: NSView, height: CGFloat) -> NSView? {
        #if compiler(>=6.2)
        if #available(macOS 26, *) {
            let glass = NSGlassEffectView()
            glass.contentView = content
            glass.cornerRadius = height / 2
            glass.translatesAutoresizingMaskIntoConstraints = false
            // Size the capsule from its content, so it hugs the text.
            content.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([
                content.leadingAnchor.constraint(equalTo: glass.leadingAnchor),
                content.trailingAnchor.constraint(equalTo: glass.trailingAnchor),
                content.topAnchor.constraint(equalTo: glass.topAnchor),
                content.bottomAnchor.constraint(equalTo: glass.bottomAnchor),
            ])
            glass.heightAnchor.constraint(equalToConstant: height).isActive = true
            return glass
        }
        #endif
        return nil
    }

    /// A rounded vertical strip around `content` (the inspector's tab pill):
    /// glass on macOS 26, a faint filled box before. Sized from its content.
    static func verticalCapsule(around content: NSView, width: CGFloat) -> NSView {
        let container: NSView
        #if compiler(>=6.2)
        if #available(macOS 26, *) {
            let glass = NSGlassEffectView()
            glass.contentView = content
            glass.cornerRadius = width / 2
            container = glass
        } else {
            container = plainCapsule(around: content, width: width)
        }
        #else
        container = plainCapsule(around: content, width: width)
        #endif
        container.translatesAutoresizingMaskIntoConstraints = false
        content.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            content.topAnchor.constraint(equalTo: container.topAnchor),
            content.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            container.widthAnchor.constraint(equalToConstant: width),
        ])
        return container
    }

    private static func plainCapsule(around content: NSView, width: CGFloat) -> NSView {
        let box = NSBox()
        box.boxType = .custom
        box.borderWidth = 0
        box.cornerRadius = width / 2
        box.fillColor = NSColor.labelColor.withAlphaComponent(0.07)
        box.contentViewMargins = .zero
        box.contentView = content
        return box
    }
}
