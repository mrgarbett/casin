// SPDX-License-Identifier: GPL-3.0-or-later
// OpenClone for macOS: Swift + AppKit over the shared C++ core.
//
// Finder "Open With" and double-click arrive through NSDocumentController,
// which uses the document types in Info.plist (see scripts/build_macos_app.sh).
//
// Headless check (no window):
//   OpenClone --render-map out.png <file>      (.pdf works too)
import AppKit
import OpenCloneCore

final class AppDelegate: NSObject, NSApplicationDelegate {
    private let launchFiles: [String]

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
        if launchFiles.isEmpty && NSDocumentController.shared.documents.isEmpty {
            NSDocumentController.shared.openDocument(nil)
        }
    }

    func applicationShouldOpenUntitledFile(_ sender: NSApplication) -> Bool { false }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    @objc func showAbout(_ sender: Any?) {
        let credits = NSAttributedString(string:
            "An open-source plasmid viewer. Opens GenBank, FASTA and SnapGene .dna files.\n\n" +
            "Restriction enzyme data: REBASE, the Restriction Enzyme Database (rebase.neb.com).\n" +
            "Melting temperature: SantaLucia (1998) unified nearest-neighbor parameters.")
        NSApp.orderFrontStandardAboutPanel(options: [
            .applicationName: "OpenClone",
            .applicationVersion: "0.1.0",
            .credits: credits,
        ])
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

        let about = item("About OpenClone", #selector(showAbout(_:)))
        about.target = self
        submenu("OpenClone", [
            about,
            .separator(),
            item("Hide OpenClone", #selector(NSApplication.hide(_:)), "h"),
            item("Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option]),
            item("Show All", #selector(NSApplication.unhideAllApplications(_:))),
            .separator(),
            item("Quit OpenClone", #selector(NSApplication.terminate(_:)), "q"),
        ])
        submenu("File", [
            item("Open…", #selector(NSDocumentController.openDocument(_:)), "o"),
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
            item("Find…", #selector(DocumentWindowController.performFindPanelAction(_:)), "f"),
        ])
        submenu("View", [
            item("Toggle Sidebar", #selector(NSSplitViewController.toggleSidebar(_:)), "s", [.command, .control]),
            .separator(),
            item("Show Complement Strand", #selector(DocumentWindowController.toggleComplementStrand(_:))),
            .separator(),
            item("Enter Full Screen", #selector(NSWindow.toggleFullScreen(_:)), "f", [.command, .control]),
        ])
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"))
        windowMenu.addItem(item("Zoom", #selector(NSWindow.performZoom(_:))))
        let windowItem = NSMenuItem(title: "Window", action: nil, keyEquivalent: "")
        windowItem.submenu = windowMenu
        main.addItem(windowItem)
        NSApp.windowsMenu = windowMenu
        return main
    }
}

// MARK: - Entry point

func renderMapHeadless(_ args: [String]) -> Int32 {
    guard args.count >= 2 else {
        FileHandle.standardError.write("usage: OpenClone --render-map out.png <file>\n".data(using: .utf8)!)
        return 2
    }
    let doc = CoreDocument()
    do {
        try doc.load(path: args[1])
        try writeMapImage(document: doc, to: URL(fileURLWithPath: args[0]), size: CGSize(width: 1000, height: 820))
        print("wrote \(args[0])")
        return 0
    } catch {
        FileHandle.standardError.write("OpenClone: \(error.localizedDescription)\n".data(using: .utf8)!)
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
let delegate = AppDelegate(launchFiles: files)
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
