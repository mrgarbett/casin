// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import OpenCloneCore
import UniformTypeIdentifiers

/// The window for one document: map | sequence | sidebar, a find field in the
/// toolbar and a status line at the bottom.
final class DocumentWindowController: NSWindowController, NSToolbarDelegate, NSMenuItemValidation {
    private let core: CoreDocument
    private let mapView: MapView
    private let sequenceView: SequenceView
    private let sidebar: SidebarController
    private let status = NSTextField(labelWithString: "")
    private let searchField = NSSearchField()

    init(document: SequenceDocument) {
        core = document.core
        mapView = MapView(document: core)
        sequenceView = SequenceView(document: core)
        sidebar = SidebarController(document: core)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1320, height: 820),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.minSize = NSSize(width: 820, height: 480)
        window.setFrameAutosaveName("OpenCloneDocumentWindow")
        super.init(window: window)
        buildContent()
        buildToolbar()
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
                                               queue: .main) { [weak self] _ in
            self?.sequenceView.updateSize()
        }

        let split = NSSplitView()
        split.isVertical = true
        split.dividerStyle = .thin
        split.addArrangedSubview(mapView)
        split.addArrangedSubview(scroll)
        split.addArrangedSubview(sidebar.view)
        split.setHoldingPriority(.defaultLow, forSubviewAt: 0)
        split.setHoldingPriority(.defaultLow - 1, forSubviewAt: 1)
        split.setHoldingPriority(.defaultHigh, forSubviewAt: 2)
        sidebar.view.widthAnchor.constraint(greaterThanOrEqualToConstant: 240).isActive = true
        mapView.widthAnchor.constraint(greaterThanOrEqualToConstant: 300).isActive = true
        scroll.widthAnchor.constraint(greaterThanOrEqualToConstant: 360).isActive = true

        status.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail

        let root = DropView()
        let stack = NSStackView(views: [split, status])
        stack.orientation = .vertical
        stack.spacing = 4
        stack.edgeInsets = NSEdgeInsets(top: 0, left: 0, bottom: 6, right: 0)
        stack.setHuggingPriority(.defaultLow, for: .vertical)
        split.setContentHuggingPriority(.defaultLow, for: .vertical)
        stack.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            stack.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor),
            stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            split.widthAnchor.constraint(equalTo: stack.widthAnchor),
            status.leadingAnchor.constraint(equalTo: stack.leadingAnchor, constant: 12),
        ])
        window.contentView = root
        DispatchQueue.main.async {
            split.setPosition(window.frame.width * 0.40, ofDividerAt: 0)
            split.setPosition(window.frame.width - 280, ofDividerAt: 1)
        }
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
        window?.title = core.name.isEmpty ? "Untitled" : core.name
        window?.subtitle = "\(core.length.formatted()) bp · \(core.isCircular ? "circular" : "linear")"
    }

    // MARK: Toolbar

    private let searchItem = NSToolbarItem.Identifier("find")

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.flexibleSpace, searchItem]
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.flexibleSpace, searchItem]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        guard id == searchItem else { return nil }
        let item = NSSearchToolbarItem(itemIdentifier: id)
        item.searchField = searchField
        item.preferredWidthForSearchField = 240
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
