// SPDX-License-Identifier: GPL-3.0-or-later
// A launch check for CI, where nobody can click:
//   Restricto --ui-smoke <out-dir> <file>
// opens <file> in a real window, then resizes it to its minimum size, below
// it (as a restored frame can be), collapses and reopens the sidebar, stacks
// and hides the map, walks the right panel's panes (the sequence among them
// with the map beside it), hides and shows the inspector, opens Find (which
// reopens a hidden panel on the sequence), selects bases (the status splits),
// copies the map image, adds a His-tagged primer in the Primers
// pane, opens a PCR product from the PCR wizard, runs a gel in the gel wizard, and saves a PNG of the window after each step. It exits 0 when every step got
// through layout; a layout loop or other exception aborts the process instead.
import AppKit
import RestrictoCore

enum UISmokeTest {
    static func startIfRequested() {
        let args = CommandLine.arguments
        guard let i = args.firstIndex(of: "--ui-smoke"), i + 1 < args.count else { return }
        let out = URL(fileURLWithPath: args[i + 1], isDirectory: true)
        try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)
        // A crash prints its stack, as CI keeps no crash reports.
        for sig in [SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT] {
            signal(sig) { code in
                var frames = [UnsafeMutableRawPointer?](repeating: nil, count: 80)
                let count = backtrace(&frames, Int32(frames.count))
                let header = "ui-smoke: crashed with signal \(code)\n"
                _ = header.withCString { write(2, $0, strlen($0)) }
                backtrace_symbols_fd(&frames, count, 2)
                _exit(128 + code)
            }
        }
        // Give up rather than hang CI if the window never appears. The whole run
        // now takes about a minute on CI, so leave it room.
        DispatchQueue.main.asyncAfter(deadline: .now() + 150) {
            FileHandle.standardError.write("ui-smoke: timed out\n".data(using: .utf8)!)
            exit(3)
        }
        waitForWindow(out: out, tries: 40)
    }

    private static func waitForWindow(out: URL, tries: Int) {
        if let window = NSApp.windows.first(where: { $0.windowController is DocumentWindowController }) {
            run(steps(window, out: out), out: out)
        } else if tries > 0 {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { waitForWindow(out: out, tries: tries - 1) }
        } else {
            FileHandle.standardError.write("ui-smoke: no document window\n".data(using: .utf8)!)
            exit(2)
        }
    }

    private static func steps(_ window: NSWindow, out: URL) -> [(String, () -> Void)] {
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
            ("9h-primer-fit-tm", {
                // Fitting to a hotter Tm grows the annealing part at its 5' end only.
                let core = (controller?.document as? SequenceDocument)?.core
                core?.select(start: 500, length: 22)
                controller?.addPrimer(nil)
                let sites = PrimerEditor.fitAll(to: 70)
                guard let s = sites.first, s.start + s.length == 522, s.length > 22,
                      let core = core, oc_document_has_pending_primer(core.handle) else {
                    FileHandle.standardError.write("ui-smoke: fitting a primer to a Tm failed\n".data(using: .utf8)!)
                    exit(5)
                }
            }),
            ("9h-primer-cancelled", {
                PrimerEditor.cancelAll()
                let core = (controller?.document as? SequenceDocument)?.core
                guard let core = core, !oc_document_has_pending_primer(core.handle), !PrimerEditor.isAnyOpen else {
                    FileHandle.standardError.write("ui-smoke: cancelling left a primer behind\n".data(using: .utf8)!)
                    exit(5)
                }
            }),
            ("9i-pcr-wizard", {
                // Sequence > PCR… with the sample's two primers.
                controller?.pcrProduct(nil)
                guard PcrWizard.isAnyOpen, let length = PcrWizard.productLengths.first, length > 0,
                      PcrWizard.programSteps.first == 6 else {
                    FileHandle.standardError.write("ui-smoke: the PCR wizard showed no product\n".data(using: .utf8)!)
                    exit(6)
                }
            }),
            ("9i-pcr-pick", {
                // Clicking primers on the vector map: a plain click starts over
                // with one primer (no product), shift-click adds the second.
                let primers = (controller?.document as? SequenceDocument)?.core.primers.filter { $0.length > 0 } ?? []
                let expected = PcrWizard.productLengths.first ?? 0
                guard let f = primers.first(where: { $0.strand == Int32(OC_STRAND_FORWARD) }),
                      let r = primers.first(where: { $0.strand == Int32(OC_STRAND_REVERSE) }) else {
                    FileHandle.standardError.write("ui-smoke: the sample has no primer pair\n".data(using: .utf8)!)
                    exit(6)
                }
                PcrWizard.pickOnMap(f.index, adding: false)
                let one = PcrWizard.productLengths.first ?? -1
                let tabOff = !PcrWizard.productTabEnabled
                PcrWizard.pickOnMap(r.index, adding: true)
                guard one == 0, tabOff, PcrWizard.productLengths.first == expected, PcrWizard.productTabEnabled else {
                    FileHandle.standardError.write("ui-smoke: picking primers on the PCR map went wrong\n".data(using: .utf8)!)
                    exit(6)
                }
            }),
            ("9i-pcr-opened", {
                func note(_ text: String) { print("ui-smoke: 9i \(text)"); fflush(stdout) }
                note("capture")
                if let wizard = PcrWizard.openWindows.first {
                    capture(wizard, to: out.appendingPathComponent("9i-pcr-wizard-window.png"))
                }
                let expected = PcrWizard.productLengths.first ?? 0
                let before = Set(NSDocumentController.shared.documents.map { ObjectIdentifier($0) })
                note("open product")
                PcrWizard.openAll()
                note("opened")
                let added = NSDocumentController.shared.documents.filter { !before.contains(ObjectIdentifier($0)) }
                guard added.count == 1, let product = added.first as? SequenceDocument,
                      product.core.length == expected, !PcrWizard.isAnyOpen else {
                    FileHandle.standardError.write("ui-smoke: Open Product didn't open the PCR product\n".data(using: .utf8)!)
                    exit(6)
                }
                note("close product")
                product.close()
                note("closed")
                window.makeKeyAndOrderFront(nil)
            }),
            ("9k-gel-wizard", {
                // Cloning > Digest and Gel…: the ladder, the plasmid uncut (two
                // bands, nicked and supercoiled), and cut with the first enzyme
                // shown; then a lane cut with EcoRI.
                controller?.digestAndGel(nil)
                let length = (controller?.document as? SequenceDocument)?.core.length ?? 0
                let first = GelWizard.bands
                GelWizard.addLane(enzymes: ["ecori"])
                let after = GelWizard.bands
                guard GelWizard.isAnyOpen, first.count >= 2, first[0].count > 5, first[1] == [length, length],
                      after.count == first.count + 1, let cut = after.last, cut.count >= 1,
                      cut.reduce(0, +) == length else {
                    FileHandle.standardError.write("ui-smoke: the gel wizard showed the wrong bands \(after)\n"
                                                       .data(using: .utf8)!)
                    exit(8)
                }
            }),
            ("9k-gel-agarose", {
                // A 2% gel holds the uncut plasmid back more than a 0.7% one;
                // then half a run, in black and white.
                GelWizard.setAgarose(0.7)
                let thin = GelWizard.runs.dropFirst().first?.first ?? 0
                GelWizard.setAgarose(2)
                let dense = GelWizard.runs.dropFirst().first?.first ?? 0
                guard thin > 0, dense > 0, dense < thin else {
                    FileHandle.standardError.write("ui-smoke: the gel's agarose didn't change how far bands ran\n"
                                                       .data(using: .utf8)!)
                    exit(8)
                }
                GelWizard.setAgarose(1)
                GelWizard.setRun(0.5, stain: .blackAndWhite)
            }),
            ("9k-gel-half-run", {
                if let wizard = GelWizard.openWindows.first {
                    capture(wizard, to: out.appendingPathComponent("9k-gel-half-run-window.png"))
                }
                GelWizard.setRun(1, stain: .ethidiumUV)
            }),
            ("9k-gel-uv-box", { GelWizard.showLook(1) }),
            ("9k-gel-3d", {
                // The 3D tab builds its RealityKit scene on first use.
                if let wizard = GelWizard.openWindows.first {
                    capture(wizard, to: out.appendingPathComponent("9k-gel-uv-box-window.png"))
                }
                GelWizard.showLook(2)
                guard GelWizard.showing3D else {
                    FileHandle.standardError.write("ui-smoke: the gel's 3D tab didn't open\n".data(using: .utf8)!)
                    exit(8)
                }
            }),
            ("9k-gel-imager", {
                if let wizard = GelWizard.openWindows.first {
                    capture(wizard, to: out.appendingPathComponent("9k-gel-3d-window.png"))
                }
                GelWizard.showLook(0)
            }),
            ("9k-gel-closed", {
                if let wizard = GelWizard.openWindows.first {
                    capture(wizard, to: out.appendingPathComponent("9k-gel-wizard-window.png"))
                }
                GelWizard.closeAll()
                window.makeKeyAndOrderFront(nil)
            }),
            ("9j-zoomed-125", {
                // ⌘0 then ⌘+ twice: 100% → 110% → 125%, map and sequence together.
                Zoom.shared.actualSize(nil)
                Zoom.shared.zoomIn(nil)
                Zoom.shared.zoomIn(nil)
                guard abs(Zoom.level - 1.25) < 0.001 else {
                    FileHandle.standardError.write("ui-smoke: zooming in went wrong\n".data(using: .utf8)!)
                    exit(7)
                }
            }),
            ("9j-zoomed-80", { for _ in 0..<8 { Zoom.shared.zoomOut(nil) } }),
            ("9j-actual-size", {
                Zoom.shared.actualSize(nil)
                guard Zoom.level == 1 else {
                    FileHandle.standardError.write("ui-smoke: Actual Size didn't go back to 100%\n".data(using: .utf8)!)
                    exit(7)
                }
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
