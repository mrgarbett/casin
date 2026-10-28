// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit

/// The window's left sidebar: the sequence files in one folder and its
/// subfolders. Clicking a file opens it (as a tab of the window). The folder is
/// shared by every window and kept between launches; until one is chosen it is
/// the folder of the first document shown.
final class FolderBrowserController: NSViewController, NSOutlineViewDataSource, NSOutlineViewDelegate {
    /// Extensions Restricto opens; other files are left out of the list.
    static let sequenceExtensions: Set<String> = ["gb", "gbk", "genbank", "gbff", "ape", "dna", "fasta", "fa",
                                                  "fna", "fas", "seq", "embl", "ab1"]
    private static let folderKey = "BrowserFolder"
    private static let folderChanged = Notification.Name("RestrictoBrowserFolderChanged")

    /// The folder when none was chosen yet (the document's own folder).
    var fallbackFolder: () -> URL? = { nil }
    private let outline = NSOutlineView()
    private let folderLabel = NSTextField(labelWithString: "")
    private let empty = NSTextField(wrappingLabelWithString: "Choose a folder to list its sequence files here.")
    private var root: Node?
    private var opening = false

    /// One file or folder; a folder lists its children the first time it opens.
    final class Node {
        let url: URL
        let isFolder: Bool
        private var loaded: [Node]?

        init(url: URL, isFolder: Bool) {
            self.url = url
            self.isFolder = isFolder
        }

        var children: [Node] {
            if let loaded = loaded { return loaded }
            let keys: [URLResourceKey] = [.isDirectoryKey, .isPackageKey]
            let urls = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: keys,
                                                                     options: [.skipsHiddenFiles])) ?? []
            let nodes = urls.compactMap { u -> Node? in
                let values = try? u.resourceValues(forKeys: Set(keys))
                if values?.isDirectory == true && values?.isPackage != true { return Node(url: u, isFolder: true) }
                guard FolderBrowserController.sequenceExtensions.contains(u.pathExtension.lowercased()) else {
                    return nil
                }
                return Node(url: u, isFolder: false)
            }
            // Folders first, then files, each in Finder order.
            loaded = nodes.sorted {
                if $0.isFolder != $1.isFolder { return $0.isFolder }
                return $0.url.lastPathComponent.localizedStandardCompare($1.url.lastPathComponent) == .orderedAscending
            }
            return loaded ?? []
        }
    }

    override func loadView() {
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("file"))
        outline.addTableColumn(column)
        outline.outlineTableColumn = column
        outline.headerView = nil
        outline.style = .sourceList
        outline.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
        outline.dataSource = self
        outline.delegate = self
        let scroll = NSScrollView()
        scroll.documentView = outline
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false

        folderLabel.font = .systemFont(ofSize: NSFont.systemFontSize, weight: .semibold)
        folderLabel.lineBreakMode = .byTruncatingMiddle
        folderLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let choose = NSButton(image: NSImage(systemSymbolName: "folder", accessibilityDescription: "Choose Folder")
                                  ?? NSImage(size: NSSize(width: 16, height: 16)),
                              target: self, action: #selector(chooseFolder(_:)))
        choose.isBordered = false
        choose.toolTip = "Choose Folder…"
        choose.contentTintColor = .secondaryLabelColor
        empty.textColor = .secondaryLabelColor
        empty.alignment = .center

        let rootView = NSView(frame: NSRect(x: 0, y: 0, width: 240, height: 600))
        for v in [folderLabel, choose, scroll, empty] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            rootView.addSubview(v)
        }
        let guide = rootView.safeAreaLayoutGuide
        NSLayoutConstraint.activate([
            folderLabel.topAnchor.constraint(equalTo: guide.topAnchor, constant: 8),
            folderLabel.leadingAnchor.constraint(equalTo: guide.leadingAnchor, constant: 12),
            choose.centerYAnchor.constraint(equalTo: folderLabel.centerYAnchor),
            choose.leadingAnchor.constraint(greaterThanOrEqualTo: folderLabel.trailingAnchor, constant: 6),
            choose.trailingAnchor.constraint(equalTo: guide.trailingAnchor, constant: -10),
            scroll.topAnchor.constraint(equalTo: folderLabel.bottomAnchor, constant: 6),
            scroll.leadingAnchor.constraint(equalTo: guide.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: guide.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: rootView.bottomAnchor),
            empty.centerYAnchor.constraint(equalTo: guide.centerYAnchor),
            empty.leadingAnchor.constraint(equalTo: guide.leadingAnchor, constant: 16),
            empty.trailingAnchor.constraint(equalTo: guide.trailingAnchor, constant: -16),
        ])
        view = rootView

        NotificationCenter.default.addObserver(self, selector: #selector(reload), name: Self.folderChanged,
                                               object: nil)
        // Pick up files saved or added elsewhere when the user comes back.
        NotificationCenter.default.addObserver(self, selector: #selector(reload),
                                               name: NSApplication.didBecomeActiveNotification, object: nil)
    }

    override func viewWillAppear() {
        super.viewWillAppear()
        if root == nil { reload() }
    }

    /// The chosen folder, or else the fallback.
    private var folder: URL? {
        if let path = UserDefaults.standard.string(forKey: Self.folderKey),
           FileManager.default.fileExists(atPath: path) {
            return URL(fileURLWithPath: path, isDirectory: true)
        }
        return fallbackFolder()
    }

    /// Lists the folder again, keeping open subfolders open.
    @objc func reload() {
        guard isViewLoaded else { return }
        let expanded = expandedURLs()
        root = folder.map { Node(url: $0, isFolder: true) }
        folderLabel.stringValue = root.map { FileManager.default.displayName(atPath: $0.url.path) } ?? "No Folder"
        folderLabel.toolTip = root?.url.path
        empty.isHidden = root != nil
        outline.reloadData()
        expand(expanded, in: root?.children ?? [])
    }

    private func expandedURLs() -> Set<URL> {
        var urls = Set<URL>()
        for row in 0..<outline.numberOfRows {
            if let node = outline.item(atRow: row) as? Node, outline.isItemExpanded(node) { urls.insert(node.url) }
        }
        return urls
    }

    private func expand(_ urls: Set<URL>, in nodes: [Node]) {
        for node in nodes where node.isFolder && urls.contains(node.url) {
            outline.expandItem(node)
            expand(urls, in: node.children)
        }
    }

    @objc func chooseFolder(_ sender: Any?) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Show"
        panel.directoryURL = folder
        let done: (NSApplication.ModalResponse) -> Void = { response in
            guard response == .OK, let url = panel.url else { return }
            UserDefaults.standard.set(url.path, forKey: Self.folderKey)
            NotificationCenter.default.post(name: Self.folderChanged, object: nil)
        }
        if let window = view.window { panel.beginSheetModal(for: window, completionHandler: done) }
        else { done(panel.runModal()) }
    }

    // MARK: NSOutlineViewDataSource / Delegate

    func outlineView(_ outlineView: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        ((item as? Node) ?? root)?.children.count ?? 0
    }

    func outlineView(_ outlineView: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        ((item as? Node) ?? root)!.children[index]
    }

    func outlineView(_ outlineView: NSOutlineView, isItemExpandable item: Any) -> Bool {
        (item as? Node)?.isFolder ?? false
    }

    func outlineView(_ outlineView: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any) -> NSView? {
        guard let node = item as? Node else { return nil }
        let icon = NSImageView(image: NSWorkspace.shared.icon(forFile: node.url.path))
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.widthAnchor.constraint(equalToConstant: 16).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 16).isActive = true
        let label = NSTextField(labelWithString: node.url.lastPathComponent)
        label.lineBreakMode = .byTruncatingMiddle
        label.toolTip = node.url.path
        let stack = NSStackView(views: [icon, label])
        stack.spacing = 6
        return stack
    }

    func outlineViewSelectionDidChange(_ notification: Notification) {
        guard !opening, let node = outline.item(atRow: outline.selectedRow) as? Node, !node.isFolder else { return }
        opening = true
        defer { opening = false }
        NSDocumentController.shared.openDocument(withContentsOf: node.url, display: true) { _, _, error in
            if let error = error { NSApp.presentError(error) }
        }
    }
}
