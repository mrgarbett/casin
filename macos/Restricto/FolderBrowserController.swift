// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit

/// The window's left sidebar: open documents with unsaved changes ("Not
/// Saved"), then the sequence files in one folder and its subfolders, which a
/// search field narrows to the matching files. Clicking a file opens it (as a
/// tab of the window); clicking an unsaved document shows its tab. The folder
/// is shared by every window and kept between launches; until one is chosen it
/// is the folder of the first document shown. A red dot marks unsaved changes.
final class FolderBrowserController: NSViewController, NSOutlineViewDataSource, NSOutlineViewDelegate {
    /// Posted when a document opens, closes or gains or loses unsaved changes.
    static let documentsChanged = Notification.Name("RestrictoDocumentsChanged")

    /// Extensions Restricto opens; other files are left out of the list.
    static let sequenceExtensions: Set<String> = ["gb", "gbk", "genbank", "gbff", "ape", "dna", "fasta", "fa",
                                                  "fna", "fas", "seq", "embl", "ab1"]
    private static let folderKey = "BrowserFolder"
    private static let folderChanged = Notification.Name("RestrictoBrowserFolderChanged")

    /// The folder when none was chosen yet (the document's own folder).
    var fallbackFolder: () -> URL? = { nil }
    private let outline = NSOutlineView()
    private let folderLabel = NSTextField(labelWithString: "")
    private let search = NSSearchField()
    /// Section headers (group rows).
    private let unsavedHeader = Header(title: "Not Saved")
    private let filesHeader = Header(title: "Files")
    private var unsaved: [OpenDocument] = []
    /// The folders open in the tree, kept while a search lists matches instead.
    private var treeExpanded = Set<URL>()
    /// True while the outline shows search results rather than the tree.
    private var listsMatches = false
    /// The files matching the search, or nil when there is no search.
    private var matches: [Node]?
    private let empty = NSTextField(wrappingLabelWithString: "Choose a folder to list its sequence files here.")
    private var root: Node?
    private var opening = false

    final class Header {
        let title: String
        init(title: String) { self.title = title }
    }

    /// An open document with unsaved changes.
    final class OpenDocument {
        weak var document: NSDocument?
        init(_ document: NSDocument) { self.document = document }
    }

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
            let nodes = urls.compactMap { (u: URL) -> Node? in
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
        search.placeholderString = "Search files"
        search.controlSize = .small
        search.target = self
        search.action = #selector(searchChanged(_:))

        let rootView = Glass.sidePanelBackground(frame: NSRect(x: 0, y: 0, width: 240, height: 600))
        for v in [folderLabel, choose, search, scroll, empty] as [NSView] {
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
            search.topAnchor.constraint(equalTo: folderLabel.bottomAnchor, constant: 8),
            search.leadingAnchor.constraint(equalTo: guide.leadingAnchor, constant: 10),
            search.trailingAnchor.constraint(equalTo: guide.trailingAnchor, constant: -10),
            scroll.topAnchor.constraint(equalTo: search.bottomAnchor, constant: 6),
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
        NotificationCenter.default.addObserver(self, selector: #selector(documentsChanged),
                                               name: Self.documentsChanged, object: nil)
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
        root = folder.map { Node(url: $0, isFolder: true) }
        folderLabel.stringValue = root.map { FileManager.default.displayName(atPath: $0.url.path) } ?? "No Folder"
        folderLabel.toolTip = root?.url.path
        empty.isHidden = root != nil
        updateMatches()
        documentsChanged()
    }

    /// Rebuilds the Not Saved section and the red dots, keeping the files listed.
    @objc private func documentsChanged() {
        guard isViewLoaded else { return }
        // Remember the open folders only while the tree is what's listed, so
        // clearing a search brings back the folders that were open before it.
        if !listsMatches { treeExpanded = expandedURLs() }
        unsaved = NSDocumentController.shared.documents
            .filter { doc in
                // An empty untitled window has nothing to lose.
                if let seq = doc as? SequenceDocument, seq.core.isEmpty { return false }
                return doc.isDocumentEdited || doc.fileURL == nil
            }
            .map { OpenDocument($0) }
        outline.reloadData()
        for header in topLevel { outline.expandItem(header) }
        listsMatches = matches != nil
        if !listsMatches { expand(treeExpanded, in: root?.children ?? []) }
    }

    private var topLevel: [Header] { unsaved.isEmpty ? [filesHeader] : [unsavedHeader, filesHeader] }

    /// The files in the folder and its subfolders whose name contains the search.
    private func updateMatches() {
        let query = search.stringValue.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty, let root = root else { matches = nil; return }
        var found: [Node] = []
        let keys: [URLResourceKey] = [.isDirectoryKey]
        let walker = FileManager.default.enumerator(at: root.url, includingPropertiesForKeys: keys,
                                                    options: [.skipsHiddenFiles, .skipsPackageDescendants])
        while let url = walker?.nextObject() as? URL, found.count < 500 {
            if (try? url.resourceValues(forKeys: Set(keys)))?.isDirectory == true { continue }
            guard Self.sequenceExtensions.contains(url.pathExtension.lowercased()),
                  url.lastPathComponent.range(of: query, options: [.caseInsensitive, .diacriticInsensitive]) != nil
            else { continue }
            found.append(Node(url: url, isFolder: false))
        }
        matches = found.sorted {
            $0.url.lastPathComponent.localizedStandardCompare($1.url.lastPathComponent) == .orderedAscending
        }
    }

    @objc private func searchChanged(_ sender: NSSearchField) {
        updateMatches()
        documentsChanged()
    }

    /// True when an open document for `url` has unsaved changes.
    private func isEdited(_ url: URL) -> Bool {
        NSDocumentController.shared.documents.contains {
            $0.isDocumentEdited && $0.fileURL?.standardizedFileURL == url.standardizedFileURL
        }
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

    private func children(of item: Any?) -> [Any] {
        guard let item = item else { return topLevel }
        switch item {
        case let header as Header where header === unsavedHeader: return unsaved
        case is Header: return matches ?? root?.children ?? []
        case let node as Node: return node.children
        default: return []
        }
    }

    func outlineView(_ outlineView: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        children(of: item).count
    }

    func outlineView(_ outlineView: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        children(of: item)[index]
    }

    func outlineView(_ outlineView: NSOutlineView, isItemExpandable item: Any) -> Bool {
        item is Header || (item as? Node)?.isFolder == true
    }

    func outlineView(_ outlineView: NSOutlineView, isGroupItem item: Any) -> Bool { item is Header }

    func outlineView(_ outlineView: NSOutlineView, shouldSelectItem item: Any) -> Bool { !(item is Header) }

    func outlineView(_ outlineView: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any) -> NSView? {
        if let header = item as? Header {
            let label = NSTextField(labelWithString: header === filesHeader && matches != nil
                                    ? "Search Results" : header.title)
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
            label.textColor = .secondaryLabelColor
            return label
        }
        let url: URL?
        let name: String
        let edited: Bool
        if let open = item as? OpenDocument {
            url = open.document?.fileURL
            name = open.document?.displayName ?? "Untitled"
            edited = true
        } else if let node = item as? Node {
            url = node.url
            name = node.url.lastPathComponent
            edited = !node.isFolder && isEdited(node.url)
        } else {
            return nil
        }
        let image = url.map { NSWorkspace.shared.icon(forFile: $0.path) }
            ?? NSImage(systemSymbolName: "doc", accessibilityDescription: nil) ?? NSImage()
        let icon = NSImageView(image: image)
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.widthAnchor.constraint(equalToConstant: 16).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 16).isActive = true
        let label = NSTextField(labelWithString: name)
        label.lineBreakMode = .byTruncatingMiddle
        label.toolTip = url?.path ?? name
        label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        var views: [NSView] = [icon]
        if edited { views.append(UnsavedDot()) }   // just left of the name
        views.append(label)
        let stack = NSStackView(views: views)
        stack.spacing = 6
        if edited, let dot = views.first(where: { $0 is UnsavedDot }) { stack.setCustomSpacing(4, after: dot) }
        return stack
    }

    func outlineViewSelectionDidChange(_ notification: Notification) {
        let item = outline.item(atRow: outline.selectedRow)
        if let open = item as? OpenDocument {
            open.document?.showWindows()
            return
        }
        guard !opening, let node = item as? Node, !node.isFolder else { return }
        opening = true
        defer { opening = false }
        NSDocumentController.shared.openDocument(withContentsOf: node.url, display: true) { _, _, error in
            if let error = error { NSApp.presentError(error) }
        }
    }
}

/// The small red dot that marks unsaved changes beside a file name.
final class UnsavedDot: NSView {
    init(diameter: CGFloat = 6) {
        super.init(frame: NSRect(x: 0, y: 0, width: diameter, height: diameter))
        wantsLayer = true
        layer?.backgroundColor = NSColor.systemRed.cgColor
        layer?.cornerRadius = diameter / 2
        translatesAutoresizingMaskIntoConstraints = false
        widthAnchor.constraint(equalToConstant: diameter).isActive = true
        heightAnchor.constraint(equalToConstant: diameter).isActive = true
        toolTip = "Unsaved changes"
        setAccessibilityLabel("Unsaved changes")
        setAccessibilityElement(true)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }
}
