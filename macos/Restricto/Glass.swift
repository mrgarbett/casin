// SPDX-License-Identifier: GPL-3.0-or-later
// Liquid Glass pieces shared by the window. The app needs macOS 26 and
// Xcode 26, so these use the glass classes directly.
import AppKit

enum Glass {
    /// Holds glass capsules that sit side by side so they blend into one
    /// shape as they come within `spacing` of each other (the status pill
    /// splitting into two when bases are selected).
    static func mergingContainer(around content: NSView, spacing: CGFloat) -> NSView {
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

    /// The backdrop of a side panel (the folder sidebar, the right panel): the
    /// desktop behind the window shows through, blurred, so the panels read
    /// as translucent rather than echoing the middle.
    static func sidePanelBackground(frame: NSRect) -> NSView {
        let view = NSVisualEffectView(frame: frame)
        view.material = .sidebar
        view.blendingMode = .behindWindow
        view.state = .followsWindowActiveState
        return view
    }

    /// A glass capsule around `content`, for a control that floats over the
    /// document.
    static func capsule(around content: NSView, height: CGFloat) -> NSView {
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

    /// A rounded vertical glass strip around `content` (the inspector's tab
    /// pill), sized from its content.
    static func verticalCapsule(around content: NSView, width: CGFloat) -> NSView {
        let container = NSGlassEffectView()
        container.contentView = content
        container.cornerRadius = width / 2
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
}
