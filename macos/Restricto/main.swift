// SPDX-License-Identifier: GPL-3.0-or-later
// Restricto for macOS: Swift + AppKit over the shared C++ core.
//
// Finder "Open With" and double-click arrive through NSDocumentController,
// which uses the document types in Info.plist (see scripts/build_macos_app.sh).
//
// Headless check (no window):
//   Restricto --render-map out.png <file>      (.pdf works too)
import AppKit
import RestrictoCore
import UniformTypeIdentifiers

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let launchFiles: [String]
    private var launched = false
    /// The UI smoke test neither reopens nor remembers the files left open.
    private let smokeTest = CommandLine.arguments.contains("--ui-smoke")
    /// The files open at the last quit, reopened at a plain launch.
    private static let lastOpenFilesKey = "LastOpenFiles"

    init(launchFiles: [String]) {
        self.launchFiles = launchFiles
    }

    func applicationWillFinishLaunching(_ notification: Notification) {
        NSApp.mainMenu = buildMainMenu()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // Files given on the command line (Finder opens arrive via the document controller).
        for path in launchFiles {
            NSDocumentController.shared.openDocument(withContentsOf: URL(fileURLWithPath: path), display: true) { _, _, error in
                if let error = error { NSApp.presentError(error) }
            }
        }
        UISmokeTest.startIfRequested()
        DispatchQueue.main.async { self.launched = true }
    }

    // AppKit asks this only when nothing was opened: a plain launch or a Dock
    // click with no windows, not a Finder double-click, whose document is
    // still loading when didFinishLaunching runs. AppKit then opens an empty,
    // untitled main window (the folder sidebar lists files to open), which
    // the first file opened replaces while it is still untouched.
    // At launch with nothing to open, the files open at the last quit come
    // back instead, as tabs; the empty window only when none of them is left.
    func applicationShouldOpenUntitledFile(_ sender: NSApplication) -> Bool {
        guard launchFiles.isEmpty || launched else { return false }
        if launched || smokeTest { return true }
        let paths = (UserDefaults.standard.stringArray(forKey: Self.lastOpenFilesKey) ?? [])
            .filter { FileManager.default.fileExists(atPath: $0) }
        for path in paths {
            // A file moved or broken since: skipped quietly.
            NSDocumentController.shared.openDocument(withContentsOf: URL(fileURLWithPath: path), display: true) { _, _, _ in }
        }
        return paths.isEmpty
    }

    func applicationWillTerminate(_ notification: Notification) {
        guard !smokeTest else { return }
        var paths: [String] = []
        for case let doc as SequenceDocument in NSDocumentController.shared.documents {
            if let path = doc.fileURL?.path, !paths.contains(path) { paths.append(path) }
        }
        UserDefaults.standard.set(paths, forKey: Self.lastOpenFilesKey)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    @objc func showAbout(_ sender: Any?) {
        let credits = NSAttributedString(string:
            "An open-source plasmid viewer. Opens GenBank, FASTA and SnapGene .dna files.\n\n" +
            "Restriction enzyme data: REBASE, the Restriction Enzyme Database (rebase.neb.com).\n" +
            "Melting temperature: SantaLucia (1998) unified nearest-neighbor parameters.")
        NSApp.orderFrontStandardAboutPanel(options: [
            .applicationName: "Restricto",
            .applicationVersion: String(cString: oc_version()),
            .credits: credits,
        ])
    }

    // MARK: Open Recent and window tabs

    /// File > Open Recent, filled from the document controller's list each
    /// time it opens, rather than relying on AppKit to find a menu built in code.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        for url in NSDocumentController.shared.recentDocumentURLs {
            let item = NSMenuItem(title: FileManager.default.displayName(atPath: url.path),
                                  action: #selector(openRecent(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = url
            item.image = NSWorkspace.shared.icon(forFile: url.path)
            item.image?.size = NSSize(width: 16, height: 16)
            item.toolTip = url.path
            menu.addItem(item)
        }
        if menu.numberOfItems > 0 { menu.addItem(.separator()) }
        let clear = NSMenuItem(title: "Clear Menu", action: #selector(clearRecent(_:)), keyEquivalent: "")
        clear.target = self
        menu.addItem(clear)
    }

    @objc private func openRecent(_ sender: NSMenuItem) {
        guard let url = sender.representedObject as? URL else { return }
        NSDocumentController.shared.openDocument(withContentsOf: url, display: true) { _, _, error in
            if let error = error { NSApp.presentError(error) }
        }
    }

    @objc private func clearRecent(_ sender: Any?) {
        NSDocumentController.shared.clearRecentDocuments(sender)
    }

    /// ⌘1 to ⌘8 show that tab of the front window, ⌘9 the last one.
    @objc func selectTab(_ sender: NSMenuItem) {
        guard let tabs = frontTabs, !tabs.isEmpty else { NSSound.beep(); return }
        let i = sender.tag == 9 ? tabs.count - 1 : sender.tag - 1
        guard i < tabs.count else { NSSound.beep(); return }
        tabs[i].makeKeyAndOrderFront(nil)
    }

    private var frontTabs: [NSWindow]? {
        guard let window = NSApp.mainWindow else { return nil }
        return window.tabbedWindows ?? [window]
    }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        if menuItem.action == #selector(selectTab(_:)) {
            let count = frontTabs?.count ?? 0
            return menuItem.tag == 9 ? count > 0 : menuItem.tag <= count
        }
        if menuItem.action == #selector(clearRecent(_:)) {
            return !NSDocumentController.shared.recentDocumentURLs.isEmpty
        }
        return true
    }

    private func buildMainMenu() -> NSMenu {
        let main = NSMenu()

        func submenu(_ title: String, _ items: [NSMenuItem]) {
            let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            let menu = NSMenu(title: title)
            items.forEach(menu.addItem)
            item.submenu = menu
            main.addItem(item)
        }
        func item(_ title: String, _ action: Selector?, _ key: String = "",
                  _ modifiers: NSEvent.ModifierFlags = .command) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
            i.keyEquivalentModifierMask = modifiers
            return i
        }

        let about = item("About Restricto", #selector(showAbout(_:)))
        about.target = self
        submenu("Restricto", [
            about,
            .separator(),
            item("Hide Restricto", #selector(NSApplication.hide(_:)), "h"),
            item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option]),
            item("Show All", #selector(NSApplication.unhideAllApplications(_:))),
            .separator(),
            item("Quit Restricto", #selector(NSApplication.terminate(_:)), "q"),
        ])
        let recent = NSMenuItem(title: "Open Recent", action: nil, keyEquivalent: "")
        recent.submenu = NSMenu(title: "Open Recent")
        recent.submenu?.delegate = self
        if let menu = recent.submenu { menuNeedsUpdate(menu) }
        submenu("File", [
            item("Open…", #selector(NSDocumentController.openDocument(_:)), "o"),
            recent,
            item("Show Folder in Sidebar…", #selector(DocumentWindowController.chooseBrowserFolder(_:)), "o",
                 [.command, .shift]),
            .separator(),
            item("Close", #selector(NSWindow.performClose(_:)), "w"),
            item("Save As GenBank…", #selector(DocumentWindowController.saveAsGenBank(_:)), "s", [.command, .shift]),
            item("Export FASTA…", #selector(DocumentWindowController.exportFasta(_:))),
            item("Export Map…", #selector(DocumentWindowController.exportMap(_:)), "e"),
        ])
        submenu("Edit", [
            item("Copy", #selector(DocumentWindowController.copy(_:)), "c"),
            item("Copy Reverse Complement", #selector(DocumentWindowController.copyReverseComplement(_:)), "c",
                 [.command, .shift]),
            item("Select All", #selector(NSText.selectAll(_:)), "a"),
            .separator(),
            item("Find…", #selector(DocumentWindowController.toggleFindBar(_:)), "f"),
        ])
        submenu("View", [
            item("Toggle Sidebar", #selector(NSSplitViewController.toggleSidebar(_:)), "s", [.command, .control]),
            item("Hide Inspector", #selector(DocumentWindowController.toggleInspectorPanel(_:)), "i",
                 [.command, .option]),
            .separator(),
            item("Hide Map", #selector(DocumentWindowController.toggleMapHidden(_:)), "m", [.command, .shift]),
            item("Stack Map Above Sequence", #selector(DocumentWindowController.toggleMapStacked(_:))),
            .separator(),
            item("Show Complement Strand", #selector(DocumentWindowController.toggleComplementStrand(_:))),
            item("Group Bases in Tens", #selector(DocumentWindowController.toggleGroupTens(_:))),
            .separator(),
            item("Enter Full Screen", #selector(NSWindow.toggleFullScreen(_:)), "f", [.command, .control]),
        ])
        // Zoom In, Zoom Out and Actual Size, before Enter Full Screen.
        if let view = main.item(withTitle: "View")?.submenu {
            let at = view.numberOfItems - 1
            for (i, zoomItem) in (Zoom.menuItems() + [NSMenuItem.separator()]).enumerated() {
                view.insertItem(zoomItem, at: at + i)
            }
        }
        ToolMenus.install(in: main)
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"))
        windowMenu.addItem(item("Zoom", #selector(NSWindow.performZoom(_:))))
        windowMenu.addItem(.separator())
        for n in 1...9 {
            let tab = item(n == 9 ? "Show Last Tab" : "Show Tab \(n)", #selector(selectTab(_:)), "\(n)")
            tab.target = self
            tab.tag = n
            windowMenu.addItem(tab)
        }
        let windowItem = NSMenuItem(title: "Window", action: nil, keyEquivalent: "")
        windowItem.submenu = windowMenu
        main.addItem(windowItem)
        NSApp.windowsMenu = windowMenu
        return main
    }
}

/// Another installed app (ApE, SnapGene, MacVector, ...) may export its own UTI
/// for .gb or .dna; Launch Services then prefers it over our imported types.
/// Info.plist lists those UTIs too so Finder offers Restricto, but documents
/// always take our type by filename extension, so that Save picks the right
/// format (SequenceDocument.write switches on our type names).
final class DocumentController: NSDocumentController {
    override func typeForContents(of url: URL) throws -> String {
        let type = try super.typeForContents(of: url)
        if type.hasPrefix("io.github.mrgarbett.restricto.") { return type }
        let ext = url.pathExtension.lowercased()
        for ours in ["io.github.mrgarbett.restricto.genbank", "io.github.mrgarbett.restricto.snapgene-dna", "io.github.mrgarbett.restricto.fasta",
                     "io.github.mrgarbett.restricto.embl"] {
            if let ut = UTType(ours), ut.tags[.filenameExtension]?.contains(ext) == true { return ours }
        }
        return type
    }

    /// A file opened from anywhere (sidebar, Finder, Open, a drop) replaces the
    /// empty window the app starts with, while that window is still untouched.
    /// AppKit's own transient-document swap doesn't happen once windows are tabbed.
    override func openDocument(withContentsOf url: URL, display displayDocument: Bool,
                               completionHandler: @escaping (NSDocument?, Bool, Error?) -> Void) {
        let empty = documents.filter {
            guard let seq = $0 as? SequenceDocument else { return false }
            return seq.fileURL == nil && !seq.isDocumentEdited && seq.core.isEmpty
        }
        super.openDocument(withContentsOf: url, display: displayDocument) { document, wasOpen, error in
            if document != nil { empty.forEach { $0.close() } }
            completionHandler(document, wasOpen, error)
        }
    }
}

// MARK: - Entry point

func renderMapHeadless(_ args: [String]) -> Int32 {
    guard args.count >= 2 else {
        FileHandle.standardError.write("usage: Restricto --render-map out.png <file>\n".data(using: .utf8)!)
        return 2
    }
    let doc = CoreDocument()
    do {
        try doc.load(path: args[1])
        try writeMapImage(document: doc, to: URL(fileURLWithPath: args[0]), size: CGSize(width: 1000, height: 820))
        print("wrote \(args[0])")
        return 0
    } catch {
        FileHandle.standardError.write("Restricto: \(error.localizedDescription)\n".data(using: .utf8)!)
        return 1
    }
}

let arguments = Array(CommandLine.arguments.dropFirst())
if let i = arguments.firstIndex(of: "--render-map") {
    exit(renderMapHeadless(Array(arguments[(i + 1)...])))
}

// Plain file paths on the command line; skip AppKit's own -Flag value pairs.
var files: [String] = []
var skipNext = false
for arg in arguments {
    if skipNext { skipNext = false; continue }
    if arg.hasPrefix("-") { skipNext = true; continue }
    if FileManager.default.fileExists(atPath: arg) { files.append(arg) }
}

let app = NSApplication.shared
_ = DocumentController()   // the first instance becomes NSDocumentController.shared
let delegate = AppDelegate(launchFiles: files)
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
