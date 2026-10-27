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

    /// Lets the detail pane run underneath the floating glass sidebar. Its
    /// safe area then excludes the sidebar, so controls still lay out beside it.
    static func extendUnderSidebar(_ item: NSSplitViewItem) {
        #if compiler(>=6.2)
        if #available(macOS 26, *) {
            item.automaticallyAdjustsSafeAreaInsets = true
        }
        #endif
    }

    /// Wraps `content` so that it stays inside the safe area while its edges are
    /// mirrored and blurred under the sidebar and toolbar. Returns `content`
    /// itself before macOS 26.
    static func extendingBackground(of content: NSView) -> NSView {
        #if compiler(>=6.2)
        if #available(macOS 26, *) {
            let view = NSBackgroundExtensionView()
            view.contentView = content
            return view
        }
        #endif
        return content
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
}
