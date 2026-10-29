// SPDX-License-Identifier: GPL-3.0-or-later
// A launch check for CI, where nobody can click:
//   Restricto --ui-smoke <out-dir> <file>
// opens <file> in a real window, then resizes it to its minimum size, below
// it (as a restored frame can be), collapses and reopens the sidebar, stacks
// and hides the map, walks the right panel's panes (the sequence among them
// with the map beside it), hides and shows the inspector, opens Find (which
// reopens a hidden panel on the sequence), selects bases (the status splits),
// copies the map image, adds a His-tagged primer in the Primers
// pane, and saves a PNG of the window after each step. It exits 0 when every step got
// through layout; a layout loop or other exception aborts the process instead.
import AppKit
import RestrictoCore

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
        let controller = window.windowController as? DocumentWindowController
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
            ("6-map-stacked", { controller?.toggleMapStacked(nil) }),
            ("7-map-hidden", { controller?.toggleMapHidden(nil) }),
            ("8-map-shown", { controller?.toggleMapHidden(nil) }),
            ("9-map-beside", { controller?.toggleMapStacked(nil) }),
            ("9-panel-enzymes", { controller?.showInspectorPane(.enzymes) }),
            ("9-panel-primers", { controller?.showInspectorPane(.primers) }),
            ("9-panel-sequence-grouped", {
                controller?.showInspectorPane(.sequence)
                controller?.toggleGroupTens(nil)
            }),
            ("9-ungrouped", { controller?.toggleGroupTens(nil) }),
            ("9a-inspector-hidden", { controller?.toggleInspectorPanel(nil) }),
            ("9b-inspector-shown", { controller?.toggleInspectorPanel(nil) }),
            ("9c-inspector-hidden-stacked", {
                controller?.toggleInspectorPanel(nil)
                controller?.toggleMapStacked(nil)
            }),
            ("9d-inspector-shown-beside", {
                controller?.toggleMapStacked(nil)
                controller?.toggleInspectorPanel(nil)
            }),
            ("9e-find-open", { controller?.toggleFindBar(nil) }),
            ("9e-find-closed", { controller?.toggleFindBar(nil) }),
            ("9e-find-opens-hidden-panel", {
                controller?.toggleInspectorPanel(nil)
                controller?.toggleFindBar(nil)
            }),
            ("9e-find-closed-again", { controller?.toggleFindBar(nil) }),
            ("9g-selection-split-status", {
                (controller?.document as? SequenceDocument)?.core.select(start: 99, length: 30)
            }),
            ("9g-copy-map-image", { controller?.copyMapImage(nil) }),
            ("9g-caret", { (controller?.document as? SequenceDocument)?.core.select(start: 10, length: 0) }),
            ("9e-translation-on-map", { (controller?.document as? SequenceDocument)?.core.frames = 6 }),
            ("9e-translation-off", { (controller?.document as? SequenceDocument)?.core.frames = 0 }),
            ("9h-primer-pane", {
                // Right-click > Add Primer from Selection on bases 501-522.
                (controller?.document as? SequenceDocument)?.core.select(start: 500, length: 22)
                controller?.addPrimer(nil)
            }),
            ("9h-primer-his-tail", { PrimerEditor.insertFirstTag() }),
            ("9h-primer-added", {
                let core = (controller?.document as? SequenceDocument)?.core
                let before = core?.primers.count ?? 0
                PrimerEditor.acceptAll()
                guard let core = core, core.primers.count == before + 1,
                      core.primers.last?.start == 500, core.primers.last?.length == 22,
                      core.primers.last?.bases.hasPrefix("CATCATCACCACCATCAC") == true,
                      core.primers.last?.bases.count == 40,
                      !PrimerEditor.isAnyOpen,
                      !oc_document_has_pending_primer(core.handle) else {
                    FileHandle.standardError.write("ui-smoke: adding a primer in the Primers pane failed\n".data(using: .utf8)!)
                    exit(5)
                }
                oc_document_undo(core.handle)
            }),
            ("9e-edited", { (controller?.document as? NSDocument)?.updateChangeCount(.changeDone) }),
            ("9f-saved", { (controller?.document as? NSDocument)?.updateChangeCount(.changeCleared) }),
            ("10-below-minimum", { resize(NSSize(width: 600, height: 400)) }),
            ("11-sidebar-toggled-at-quit", { split?.toggleSidebar(nil) }),
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
            let inspector = split?.splitViewItems.last?.viewController.view.frame.width ?? -1
            print("ui-smoke: \(name) \(Int(window.frame.width))x\(Int(window.frame.height)) sidebar \(sidebar)"
                  + " inspector \(inspector)")
            fflush(stdout)   // keep the log if a later step aborts
            // A side panel wider than the window means its width ran away.
            if !sidebar.isFinite || sidebar > window.frame.width || !inspector.isFinite
                || inspector > window.frame.width {
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
