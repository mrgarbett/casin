// SPDX-License-Identifier: GPL-3.0-or-later
// A launch check for CI, where nobody can click:
//   OpenClone --ui-smoke <out-dir> <file>
// opens <file> in a real window, then resizes it to its minimum size, below
// it (as a restored frame can be), collapses and reopens the sidebar, and
// saves a PNG of the window after each step. It exits 0 when every step got
// through layout; a layout loop or other exception aborts the process instead.
import AppKit

enum UISmokeTest {
    static func startIfRequested() {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: "--ui-smoke"), i + 1 < args.count else { return }
        let out = URL(fileURLWithPath: args[i + 1], isDirectory: true)
        try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
        // Give up rather than hang CI if the window never appears.
        DispatchQueue.main.asyncAfter(deadline: .now() + 60) {
            FileHandle.standardError.write("ui-smoke: timed out\n".data(using: .utf8)!)
            exit(3)
        }
        waitForWindow(out: out, tries: 40)
    }

    private static func waitForWindow(out: URL, tries: Int) {
        if let window = NSApp.windows.first(where: { $0.windowController is DocumentWindowController }) {
            run(steps(window), out: out)
        } else if tries > 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { waitForWindow(out: out, tries: tries - 1) }
        } else {
            FileHandle.standardError.write("ui-smoke: no document window\n".data(using: .utf8)!)
            exit(2)
        }
    }

    private static func steps(_ window: NSWindow) -> [(String, () -> Void)] {
        let split = window.contentViewController as? NSSplitViewController
        func resize(_ size: NSSize) {
            var frame = window.frame
            frame.size = size
            window.setFrame(frame, display: true)
        }
        // Ends below the minimum size with the sidebar toggled, so the next
        // launch restores state the layout must correct: a too-small frame and
        // a collapsed sidebar that then reopens (run the check twice to test it;
        // reopening a sidebar saved collapsed once made its width run away).
        return [
            ("1-default", {}),
            ("2-minimum", { resize(window.minSize) }),
            ("3-sidebar-toggled", { split?.toggleSidebar(nil) }),
            ("4-sidebar-toggled-back", { split?.toggleSidebar(nil) }),
            ("5-large", { resize(NSSize(width: 1400, height: 900)) }),
            ("6-below-minimum", { resize(NSSize(width: 600, height: 400)) }),
            ("7-sidebar-toggled-at-quit", { split?.toggleSidebar(nil) }),
        ]
    }

    private static func run(_ steps: [(String, () -> Void)], out: URL) {
        guard let step = steps.first,
              let window = NSApp.windows.first(where: { $0.windowController is DocumentWindowController }) else {
            print("ui-smoke: ok")
            exit(0)
        }
        let name = step.0
        step.1()
        window.layoutIfNeeded()
        // Let a few display cycles run, as a real launch would.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            capture(window, to: out.appendingPathComponent("\(name).png"))
            let split = window.contentViewController as? NSSplitViewController
            let sidebar = split?.splitViewItems.first?.viewController.view.frame.width ?? -1
            print("ui-smoke: \(name) \(Int(window.frame.width))x\(Int(window.frame.height)) sidebar \(sidebar)")
            fflush(stdout)   // keep the log if a later step aborts
            // A sidebar wider than the window means its width ran away.
            if !sidebar.isFinite || sidebar > window.frame.width {
                FileHandle.standardError.write("ui-smoke: sidebar width ran away\n".data(using: .utf8)!)
                exit(4)
            }
            run(Array(steps.dropFirst()), out: out)
        }
    }

    private static func capture(_ window: NSWindow, to url: URL) {
        // The frame view includes the titlebar and toolbar.
        guard let view = window.contentView?.superview ?? window.contentView,
              let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?.write(to: url)
    }
}
