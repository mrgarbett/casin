// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import OpenCloneCore
import UniformTypeIdentifiers

/// The window for one document: a Features/Enzymes sidebar, then map |
/// sequence, with find and view controls in a unified toolbar and a status line.
///
/// Ventura through Sequoia get the classic layout: a vibrant sidebar and a flat
/// status line under the content. macOS 26 adds Liquid Glass (see Glass.swift):
/// the sidebar floats as glass with the map and sequence running underneath it,
/// and the status line becomes a glass capsule floating over the sequence.
final class DocumentWindowController: NSWindowController, NSToolbarDelegate, NSMenuItemValidation {
    private let core: CoreDocument
    private let mapView: MapView
    private let sequenceView: SequenceView
    private let sidebar: SidebarController
    private let status = NSTextField(labelWithString: "")
    private let searchField = NSSearchField()
    /// Keeps the glass status inside the sequence pane. A constant, updated on
    /// resize, so a longer status can't push the map/sequence divider.
    private var pillMaxWidth: NSLayoutConstraint?
    /// Map | sequence.
    private let split = NSSplitView()
    private var didFirstShow = false

    init(document: SequenceDocument) {
        core = document.core
        mapView = MapView(document: core)
        sequenceView = SequenceView(document: core)
        sidebar = SidebarController(document: core)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1320, height: 820),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        super.init(window: window)
        buildContent()
        buildToolbar()
        window.setContentSize(NSSize(width: 1320, height: 820))
        window.minSize = NSSize(width: 900, height: 480)
        window.setFrameAutosaveName("OpenCloneDocumentWindow")
        core.observe { [weak self] _ in self?.updateTexts() }
        updateTexts()
        window.center()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    private func buildContent() {
        guard let window = window else { return }
        let scroll = NSScrollView()
        scroll.documentView = sequenceView
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false
        scroll.contentView.postsFrameChangedNotifications = true
        NotificationCenter.default.addObserver(forName: NSView.frameDidChangeNotification, object: scroll.contentView,
                                               queue: .main) { [weak self, weak scroll] _ in
            self?.sequenceView.updateSize()
            if let scroll = scroll { self?.pillMaxWidth?.constant = max(0, scroll.frame.width - 32) }
        }

        split.isVertical = true
        split.dividerStyle = .thin
        split.addArrangedSubview(mapView)
        split.addArrangedSubview(scroll)
        split.setHoldingPriority(.defaultLow, forSubviewAt: 0)
        split.setHoldingPriority(.defaultLow - 1, forSubviewAt: 1)
        mapView.widthAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true
        scroll.widthAnchor.constraint(greaterThanOrEqualToConstant: 360).isActive = true

        status.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let detail = NSViewController()
        let root = DropView(frame: NSRect(x: 0, y: 0, width: 1040, height: 820))
        detail.view = root
        if let pill = floatingStatus() {
            // macOS 26: content fills the pane (mirrored under the sidebar and
            // toolbar); the status floats over the bottom of the sequence.
            let content = Glass.extendingBackground(of: split)
            for v in [content, pill] {
                v.translatesAutoresizingMaskIntoConstraints = false
                root.addSubview(v)
            }
            scroll.automaticallyAdjustsContentInsets = false
            scroll.contentInsets = NSEdgeInsets(top: 0, left: 0, bottom: 52, right: 0)
            NSLayoutConstraint.activate([
                content.leadingAnchor.constraint(equalTo: root.leadingAnchor),
                content.trailingAnchor.constraint(equalTo: root.trailingAnchor),
                content.topAnchor.constraint(equalTo: root.topAnchor),
                content.bottomAnchor.constraint(equalTo: root.bottomAnchor),
                pill.centerXAnchor.constraint(equalTo: scroll.centerXAnchor),
                pill.bottomAnchor.constraint(equalTo: root.safeAreaLayoutGuide.bottomAnchor, constant: -12),
            ])
            pillMaxWidth = pill.widthAnchor.constraint(lessThanOrEqualToConstant: 400)
            pillMaxWidth?.isActive = true
        } else {
            // Ventura to Sequoia: content below the toolbar, flat status line under it.
            let stack = NSStackView(views: [split, status])
            stack.orientation = .vertical
            stack.spacing = 4
            stack.edgeInsets = NSEdgeInsets(top: 0, left: 0, bottom: 6, right: 0)
            stack.setHuggingPriority(.defaultLow, for: .vertical)
            split.setContentHuggingPriority(.defaultLow, for: .vertical)
            stack.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(stack)
            NSLayoutConstraint.activate([
                stack.leadingAnchor.constraint(equalTo: root.safeAreaLayoutGuide.leadingAnchor),
                stack.trailingAnchor.constraint(equalTo: root.safeAreaLayoutGuide.trailingAnchor),
                stack.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor),
                stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
                split.widthAnchor.constraint(equalTo: stack.widthAnchor),
                status.leadingAnchor.constraint(equalTo: stack.leadingAnchor, constant: 12),
                status.trailingAnchor.constraint(lessThanOrEqualTo: stack.trailingAnchor, constant: -12),
            ])
        }

        let sidebarItem = NSSplitViewItem(sidebarWithViewController: sidebar)
        sidebarItem.minimumThickness = 260
        sidebarItem.maximumThickness = 420
        sidebarItem.canCollapse = true
        let detailItem = NSSplitViewItem(viewController: detail)
        Glass.extendUnderSidebar(detailItem)

        let splitController = NSSplitViewController()
        splitController.splitView.autosaveName = "OpenCloneSidebar"
        splitController.addSplitViewItem(sidebarItem)
        splitController.addSplitViewItem(detailItem)
        window.contentViewController = splitController
    }

    /// The first time the window shows: lay it out, then set the map/sequence
    /// split from the real width and start the sequence at its first line.
    override func showWindow(_ sender: Any?) {
        super.showWindow(sender)
        guard !didFirstShow, let window = window else { return }
        didFirstShow = true
        // init ran before the document was attached, so the subtitle could
        // not yet tell the sequence name from the file name.
        updateTexts()
        window.layoutIfNeeded()
        split.setPosition(split.bounds.width * 0.45, ofDividerAt: 0)
        sequenceView.updateSize()
        sequenceView.scroll(.zero)
    }

    /// The status line inside a glass capsule, or nil before macOS 26.
    private func floatingStatus() -> NSView? {
        let inner = NSView()
        status.translatesAutoresizingMaskIntoConstraints = false
        inner.addSubview(status)
        NSLayoutConstraint.activate([
            status.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 14),
            status.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -14),
            status.centerYAnchor.constraint(equalTo: inner.centerYAnchor),
        ])
        guard let pill = Glass.capsule(around: inner, height: 30) else {
            status.removeFromSuperview()
            status.translatesAutoresizingMaskIntoConstraints = true
            return nil
        }
        // The capsule hugs the text, so the text must hold it open; the pill's
        // max-width constraint still truncates it in a narrow window.
        status.setContentCompressionResistancePriority(.defaultHigh, for: .horizontal)
        // Secondary grey washes out over the sequence text behind the glass.
        status.textColor = .labelColor
        return pill
    }

    private func buildToolbar() {
        let toolbar = NSToolbar(identifier: "OpenCloneToolbar")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        window?.toolbar = toolbar
        window?.toolbarStyle = .unified
        searchField.placeholderString = "Find bases (both strands)"
        searchField.sendsWholeSearchString = true
        searchField.target = self
        searchField.action = #selector(findFromField(_:))
    }

    private func updateTexts() {
        status.stringValue = core.statusText
        // The title stays the document's file name (NSDocument's, with the
        // proxy icon); the sequence name can differ, so it goes in the subtitle.
        var parts = ["\(core.length.formatted()) bp", core.isCircular ? "circular" : "linear"]
        let fileName = (document as? NSDocument)?.fileURL?.deletingPathExtension().lastPathComponent
        if !core.name.isEmpty && core.name != fileName { parts.insert(core.name, at: 0) }
        window?.subtitle = parts.joined(separator: " · ")
    }

    // MARK: Toolbar

    private let searchItem = NSToolbarItem.Identifier("find")
    private let complementItem = NSToolbarItem.Identifier("complement")
    private let exportItem = NSToolbarItem.Identifier("exportMap")

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator, .flexibleSpace, complementItem, exportItem, searchItem]
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator, .flexibleSpace, .space, complementItem, exportItem, searchItem]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        switch id {
        case searchItem:
            let item = NSSearchToolbarItem(itemIdentifier: id)
            item.searchField = searchField
            item.preferredWidthForSearchField = 240
            return item
        case complementItem:
            return button(id, label: "Complement", help: "Show or hide the complement strand",
                          symbol: "arrow.left.arrow.right", action: #selector(toggleComplementStrand(_:)))
        case exportItem:
            return button(id, label: "Export Map", help: "Export the map as PNG or PDF",
                          symbol: "square.and.arrow.up", action: #selector(exportMap(_:)))
        default:
            return nil
        }
    }

    /// A plain bordered toolbar button. On macOS 26 the system groups these into glass.
    private func button(_ id: NSToolbarItem.Identifier, label: String, help: String, symbol: String,
                        action: Selector) -> NSToolbarItem {
        let item = NSToolbarItem(itemIdentifier: id)
        item.label = label
        item.paletteLabel = label
        item.toolTip = help
        item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: label)
        item.isBordered = true
        item.target = self
        item.action = action
        return item
    }

    // MARK: Actions (reached through the responder chain from the menu)

    @objc private func findFromField(_ sender: NSSearchField) {
        let query = sender.stringValue.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return }
        let result = core.findNext(query)
        if result.match < 0 {
            NSSound.beep()
            status.stringValue = "Find takes DNA bases (IUPAC codes such as N and R work too)"
        } else if result.match == 0 {
            NSSound.beep()
            status.stringValue = "No match for \(query.uppercased()) on either strand"
        }
    }

    @objc func performFindPanelAction(_ sender: Any?) {
        window?.makeFirstResponder(searchField)
    }

    @objc func copy(_ sender: Any?) {
        copyToPasteboard(core.selectedBases)
    }

    @objc func copyReverseComplement(_ sender: Any?) {
        copyToPasteboard(core.selectedReverseComplement)
    }

    private func copyToPasteboard(_ text: String) {
        guard !text.isEmpty else { NSSound.beep(); return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    @objc func toggleComplementStrand(_ sender: Any?) {
        sequenceView.showComplement.toggle()
    }

    @objc func saveAsGenBank(_ sender: Any?) {
        runSavePanel(name: "\(core.name).gb", types: [UTType(filenameExtension: "gb") ?? .plainText]) { url in
            try self.core.save(path: url.path, format: Int32(OC_FORMAT_GENBANK))
        }
    }

    @objc func exportFasta(_ sender: Any?) {
        runSavePanel(name: "\(core.name).fasta", types: [UTType(filenameExtension: "fasta") ?? .plainText]) { url in
            try self.core.save(path: url.path, format: Int32(OC_FORMAT_FASTA))
        }
    }

    @objc func exportMap(_ sender: Any?) {
        let size = CGSize(width: max(900, mapView.bounds.width), height: max(760, mapView.bounds.height))
        runSavePanel(name: "\(core.name)-map.png", types: [.png, .pdf]) { url in
            try writeMapImage(document: self.core, to: url, size: size)
        }
    }

    private func runSavePanel(name: String, types: [UTType], write: @escaping (URL) throws -> Void) {
        guard let window = window, !core.isEmpty else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = name
        panel.allowedContentTypes = types
        panel.beginSheetModal(for: window) { response in
            guard response == .OK, let url = panel.url else { return }
            do {
                try write(url)
            } catch {
                NSAlert(error: error).beginSheetModal(for: window)
            }
        }
    }

    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        switch item.action {
        case #selector(copy(_:)), #selector(copyReverseComplement(_:)):
            return core.hasSelection && core.selectionLength > 0
        case #selector(toggleComplementStrand(_:)):
            item.state = sequenceView.showComplement ? .on : .off
            return true
        default:
            return true
        }
    }
}

/// Window content that opens dropped files as documents.
final class DropView: NSView {
    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { .copy }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                         options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        for url in urls {
            NSDocumentController.shared.openDocument(withContentsOf: url, display: true) { _, _, error in
                if let error = error { NSApp.presentError(error) }
            }
        }
        return !urls.isEmpty
    }
}
