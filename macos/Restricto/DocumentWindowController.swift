// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import RestrictoCore
import UniformTypeIdentifiers

/// The window for one document: a folder browser on the left, the map in the
/// middle and a Sequence/Enzymes/Features/Primers panel on the right; or, with
/// the map stacked above the sequence (or hidden), both in the middle and an
/// Enzymes/Features/Primers panel on the right. Find and view controls sit in
/// a unified toolbar and a status line.
///
/// Ventura through Sequoia get the classic layout: a vibrant sidebar and a flat
/// status line under the content. macOS 26 adds Liquid Glass (see Glass.swift):
/// the sidebar floats as glass with the map and sequence running underneath it,
/// and the status line becomes a glass capsule floating over the sequence.
final class DocumentWindowController: NSWindowController, NSToolbarDelegate, NSMenuItemValidation {
    private let core: CoreDocument
    private let mapView: MapView
    private let sequenceView: SequenceView
    /// The sequence view's scroll view; it moves between the middle and the panel.
    private let sequenceScroll = NSScrollView()
    private let browser = FolderBrowserController()
    private let inspector: InspectorController
    private var inspectorItem: NSSplitViewItem?
    private var detailItem: NSSplitViewItem?
    private var splitController: NSSplitViewController?
    /// macOS 26: the status floats over the middle, so the sequence there scrolls clear of it.
    private var floatsStatus = false
    private let status = NSTextField(labelWithString: "")
    /// View toggles beside the status: complement strand, translation, and
    /// enzymes and features on the map.
    private var viewToggles: [NSButton] = []
    private let searchField = NSSearchField()
    /// Keeps the glass status inside the sequence pane. A constant, updated on
    /// resize, so a longer status can't push the map/sequence divider.
    private var pillMaxWidth: NSLayoutConstraint?
    /// Map | sequence, or map over sequence (View > Stack Map Above Sequence).
    private let split = MapSplitView()
    /// The floating status capsule (macOS 26), and its bottom edge: under the
    /// map when the map is stacked over the sequence, else the window's bottom.
    private var statusPill: NSView?
    private var pillBottom: NSLayoutConstraint?
    private var detailRoot: NSView?
    /// Room the right panel's floating pill takes over the middle.
    private static let panelPillRoom: CGFloat = 52
    /// Minimum sizes for the two panes; which pair is active follows the layout.
    private var minWidths: [NSLayoutConstraint] = []
    private var minHeights: [NSLayoutConstraint] = []
    /// The map layout new windows start with; each window can change its own.
    static let mapStackedKey = "MapStacked"
    static let mapHiddenKey = "MapHidden"
    private var mapStacked = UserDefaults.standard.bool(forKey: DocumentWindowController.mapStackedKey)
    private var mapHidden = UserDefaults.standard.bool(forKey: DocumentWindowController.mapHiddenKey)
    private var didFirstShow = false
    /// The view that mirrors content under the side panels (macOS 26), and the
    /// safe-area insets (left, right) its content was last placed for.
    private var backgroundExtension: NSView?
    private var placedInsets: (CGFloat, CGFloat) = (-1, -1)

    init(document: SequenceDocument) {
        core = document.core
        mapView = MapView(document: core)
        sequenceView = SequenceView(document: core)
        inspector = InspectorController(document: core)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1320, height: 820),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        super.init(window: window)
        buildContent()
        buildToolbar()
        window.setContentSize(NSSize(width: 1320, height: 820))
        window.minSize = NSSize(width: 900, height: 480)
        window.setFrameAutosaveName("RestrictoDocumentWindow")
        // Every document, opened or new (including tool products), joins the
        // front window as a tab rather than opening a window of its own,
        // whatever the system-wide "Prefer tabs" setting says.
        window.tabbingMode = .preferred
        window.tabbingIdentifier = "RestrictoDocument"
        core.observe { [weak self] _ in
            self?.updateTexts()
            self?.refreshToggles()
        }
        updateTexts()
        window.center()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    private func buildContent() {
        guard let window = window else { return }
        let scroll = sequenceScroll
        scroll.documentView = sequenceView
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false
        scroll.contentView.postsFrameChangedNotifications = true
        NotificationCenter.default.addObserver(forName: NSView.frameDidChangeNotification, object: scroll.contentView,
                                               queue: .main) { [weak self] _ in
            self?.sequenceView.updateSize()
            // This runs inside the window's layout pass; changing a constraint
            // here marks layout dirty again and can loop until AppKit aborts.
            // Apply the new width after the pass, and only when it changed.
            DispatchQueue.main.async {
                guard let self = self, let limit = self.pillMaxWidth else { return }
                let width = max(0, self.split.frame.width - 32)
                if abs(limit.constant - width) >= 1 { limit.constant = width }
            }
        }

        split.isVertical = true
        split.addArrangedSubview(mapView)
        split.addArrangedSubview(scroll)
        split.setHoldingPriority(.defaultLow, forSubviewAt: 0)
        split.setHoldingPriority(.defaultLow - 1, forSubviewAt: 1)
        minWidths = [mapView.widthAnchor.constraint(greaterThanOrEqualToConstant: 260),
                     scroll.widthAnchor.constraint(greaterThanOrEqualToConstant: 320)]
        minHeights = [mapView.heightAnchor.constraint(greaterThanOrEqualToConstant: 180),
                      scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 180)]

        status.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let detail = NSViewController()
        let root = DropView(frame: NSRect(x: 0, y: 0, width: 1040, height: 820))
        detail.view = root
        detailRoot = root
        if let pill = floatingStatus() {
            // macOS 26: content fills the pane (mirrored under the sidebar and
            // toolbar); the status floats over the bottom of the sequence.
            let content = Glass.extendingBackground(of: split)
            if content !== split { backgroundExtension = content }
            for v in [content, pill] {
                v.translatesAutoresizingMaskIntoConstraints = false
                root.addSubview(v)
            }
            floatsStatus = true
            scroll.automaticallyAdjustsContentInsets = false
            // Centred in the visible middle (the safe area leaves out the
            // sidebar the content runs under), so it sits centred below the map.
            NSLayoutConstraint.activate([
                content.leadingAnchor.constraint(equalTo: root.leadingAnchor),
                content.trailingAnchor.constraint(equalTo: root.trailingAnchor),
                content.topAnchor.constraint(equalTo: root.topAnchor),
                content.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            ])
            // Shifted as the map is, which leaves room for the panel's pill on the
            // left. The edges stay inside the visible middle: on a narrow middle
            // the status text truncates instead of the pill running under the
            // sidebar or the panel pill.
            let centre = pill.centerXAnchor.constraint(equalTo: root.safeAreaLayoutGuide.centerXAnchor,
                                                       constant: Self.panelPillRoom / 2)
            centre.priority = .defaultHigh
            NSLayoutConstraint.activate([
                centre,
                pill.leadingAnchor.constraint(greaterThanOrEqualTo: root.safeAreaLayoutGuide.leadingAnchor,
                                              constant: Self.panelPillRoom + 8),
                pill.trailingAnchor.constraint(lessThanOrEqualTo: root.safeAreaLayoutGuide.trailingAnchor,
                                               constant: -8),
            ])
            statusPill = pill
            pillMaxWidth = pill.widthAnchor.constraint(lessThanOrEqualToConstant: 400)
            pillMaxWidth?.isActive = true
        } else {
            // Ventura to Sequoia: content below the toolbar, flat status line under it.
            let statusRow = NSStackView(views: [makeToggles(), status])
            statusRow.spacing = 10
            let stack = NSStackView(views: [split, statusRow])
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
                statusRow.leadingAnchor.constraint(equalTo: stack.leadingAnchor, constant: 8),
                statusRow.trailingAnchor.constraint(lessThanOrEqualTo: stack.trailingAnchor, constant: -12),
            ])
        }

        // The panel's pill floats at the top left of the middle, beside the
        // folder sidebar, and stays put whether the panels are shown or hidden.
        let panelPill = inspector.pill
        root.addSubview(panelPill)
        NSLayoutConstraint.activate([
            panelPill.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor, constant: 10),
            panelPill.leadingAnchor.constraint(equalTo: root.safeAreaLayoutGuide.leadingAnchor, constant: 8),
        ])
        inspector.onPick = { [weak self] _, wasShowing in
            guard let item = self?.inspectorItem else { return }
            // A hidden panel opens on the pane picked; picking the pane already
            // showing hides the panel again.
            if item.isCollapsed { item.animator().isCollapsed = false }
            else if wasShowing { item.animator().isCollapsed = true }
        }
        mapView.leadingInset = Self.panelPillRoom

        browser.fallbackFolder = { [weak self] in
            (self?.document as? NSDocument)?.fileURL?.deletingLastPathComponent()
        }
        let sidebarItem = NSSplitViewItem(sidebarWithViewController: browser)
        sidebarItem.minimumThickness = 200
        sidebarItem.maximumThickness = 380
        sidebarItem.canCollapse = true
        let detailItem = NSSplitViewItem(viewController: detail)
        Glass.extendUnderSidebar(detailItem)
        self.detailItem = detailItem
        let inspectorItem = NSSplitViewItem(inspectorWithViewController: inspector)
        inspectorItem.minimumThickness = 280
        inspectorItem.maximumThickness = 460
        inspectorItem.canCollapse = true
        self.inspectorItem = inspectorItem

        let splitController = NSSplitViewController()
        Self.discardBrokenSidebarFrames()
        splitController.splitView.autosaveName = Self.panesAutosaveName
        splitController.addSplitViewItem(sidebarItem)
        splitController.addSplitViewItem(detailItem)
        splitController.addSplitViewItem(inspectorItem)
        window.contentViewController = splitController
        self.splitController = splitController
        applyMapLayout()

        // The background extension places its content inside the safe area only
        // when it lays out, which a sidebar collapsing or opening doesn't cause.
        // Ask for that layout afterwards (not from inside this layout pass), and
        // only when the inset really changed.
        if backgroundExtension != nil {
            NotificationCenter.default.addObserver(forName: NSSplitView.didResizeSubviewsNotification,
                                                   object: splitController.splitView, queue: .main) { [weak self] _ in
                DispatchQueue.main.async {
                    guard let self = self, let view = self.backgroundExtension else { return }
                    let insets = (view.safeAreaInsets.left, view.safeAreaInsets.right)
                    guard insets != self.placedInsets else { return }
                    self.placedInsets = insets
                    view.needsLayout = true
                }
            }
        }
    }

    /// Builds with the safe-area pinning in Glass.swift could autosave a
    /// sidebar billions of points wide, which then broke every launch. Forget
    /// saved frames wider than any screen so the split view starts fresh.
    private static let panesAutosaveName = "RestrictoPanes"

    private static func discardBrokenSidebarFrames() {
        let key = "NSSplitView Subview Frames \(panesAutosaveName)"
        guard let frames = UserDefaults.standard.stringArray(forKey: key) else { return }
        let broken = frames.contains { frame in
            let parts = frame.split(separator: ",").compactMap { Double($0.trimmingCharacters(in: .whitespaces)) }
            return parts.count < 4 || parts[2] > 100_000 || !parts[2].isFinite
        }
        if broken { UserDefaults.standard.removeObject(forKey: key) }
    }

    /// The first time the window shows: lay it out, then set the map/sequence
    /// split from the real width and start the sequence at its first line.
    override func showWindow(_ sender: Any?) {
        super.showWindow(sender)
        guard !didFirstShow, let window = window else { return }
        didFirstShow = true
        // Keep the tab bar up even with one document, so the file name and its
        // unsaved-changes dot are always in view.
        if let group = window.tabGroup, !group.isTabBarVisible { window.toggleTabBar(nil) }
        // init ran before the document was attached, so the subtitle could
        // not yet tell the sequence name from the file name.
        updateTexts()
        window.layoutIfNeeded()
        placeDivider()
        sequenceView.updateSize()
        sequenceView.scroll(.zero)
    }

    /// With the map beside the sequence, the sequence lives in the right panel
    /// (its Sequence pane); stacked or without the map, it is in the middle.
    private var sequenceInPanel: Bool { !mapStacked && !mapHidden }

    /// Puts the window in its current layout: map in the middle and sequence in
    /// the panel, or map over sequence (or the sequence alone) in the middle.
    /// Only the minimum sizes along the split's axis apply, and none for a
    /// hidden map, so it can collapse.
    private func applyMapLayout() {
        NSLayoutConstraint.deactivate(minWidths + minHeights)
        split.isVertical = !mapStacked
        mapView.isHidden = mapHidden
        if sequenceInPanel {
            if sequenceScroll.superview === split {
                split.removeArrangedSubview(sequenceScroll)
                sequenceScroll.removeFromSuperview()
            }
            inspector.hostSequence(sequenceScroll)
            NSLayoutConstraint.activate([minWidths[0]])
        } else {
            inspector.hostSequence(nil)
            if sequenceScroll.superview !== split {
                split.addArrangedSubview(sequenceScroll)
                split.setHoldingPriority(.defaultLow - 1, forSubviewAt: 1)
            }
            let mins = mapStacked ? minHeights : minWidths
            NSLayoutConstraint.activate(mapHidden ? [mins[1]] : mins)
        }
        // The floating status sits under the map when the map is stacked over
        // the sequence, else at the bottom, where the sequence scrolls clear of it.
        let statusUnderMap = mapStacked && !mapHidden
        sequenceScroll.contentInsets = NSEdgeInsets(top: 0, left: 0,
                                                    bottom: floatsStatus && !sequenceInPanel && !statusUnderMap ? 52 : 0,
                                                    right: 0)
        if let pill = statusPill, let root = detailRoot {
            pillBottom?.isActive = false
            pillBottom = statusUnderMap
                ? pill.bottomAnchor.constraint(equalTo: mapView.bottomAnchor, constant: -10)
                : pill.bottomAnchor.constraint(equalTo: root.safeAreaLayoutGuide.bottomAnchor, constant: -12)
            pillBottom?.isActive = true
        }
        // In the middle, the sequence leaves room for the panel's floating pill.
        sequenceView.leadingInset = sequenceInPanel ? 0 : Self.panelPillRoom
        // A panel holding the sequence may grow as wide as the user likes, but
        // not over the middle: the split view (not the constraints inside the
        // middle) has to keep room for the map, or for the sequence when the
        // middle shows it, so the panel gives way on a narrow window too.
        inspectorItem?.maximumThickness = sequenceInPanel ? NSSplitViewItem.unspecifiedDimension : 460
        detailItem?.minimumThickness = sequenceInPanel ? 260 : 320
        split.adjustSubviews()
        sequenceView.updateSize()
    }

    /// Map 45 %, sequence the rest, along the split's axis; with the sequence in
    /// the panel, the panel opens to about half of the space beside the sidebar.
    private func placeDivider() {
        if sequenceInPanel, let item = inspectorItem, let outer = splitController?.splitView {
            if item.isCollapsed { item.isCollapsed = false }
            outer.layoutSubtreeIfNeeded()
            let panel = item.viewController.view.frame.width
            let sidebar = splitController?.splitViewItems.first.map { $0.isCollapsed ? 0 : $0.viewController.view.frame.width } ?? 0
            // Only a panel still sized for the tables; a width the user chose stays.
            let want = max(420, (outer.bounds.width - sidebar) * 0.5)
            if panel < 420 {
                outer.setPosition(outer.bounds.width - want - outer.dividerThickness, ofDividerAt: 1)
            }
            return
        }
        guard !mapHidden else { return }
        let length = split.isVertical ? split.bounds.width : split.bounds.height
        split.setPosition(length * 0.45, ofDividerAt: 0)
    }

    /// Shows one pane of the right panel (the UI smoke test walks through them).
    func showInspectorPane(_ pane: InspectorController.Pane) {
        inspector.select(pane)
    }

    /// The status line inside a glass capsule, or nil before macOS 26.
    private func floatingStatus() -> NSView? {
        guard Glass.isAvailable else { return nil }
        let inner = NSView()
        let toggles = makeToggles()
        for v in [toggles, status] {
            v.translatesAutoresizingMaskIntoConstraints = false
            inner.addSubview(v)
        }
        NSLayoutConstraint.activate([
            toggles.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 8),
            toggles.centerYAnchor.constraint(equalTo: inner.centerYAnchor),
            status.leadingAnchor.constraint(equalTo: toggles.trailingAnchor, constant: 10),
            status.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -14),
            status.centerYAnchor.constraint(equalTo: inner.centerYAnchor),
        ])
        guard let pill = Glass.capsule(around: inner, height: 30) else { return nil }
        // The capsule hugs the text, so the text must hold it open; the pill's
        // max-width constraint still truncates it in a narrow window.
        status.setContentCompressionResistancePriority(.defaultHigh, for: .horizontal)
        // Secondary grey washes out over the sequence text behind the glass.
        status.textColor = .labelColor
        return pill
    }

    /// Small icon toggles for the status pill (or the status row before macOS 26).
    private func makeToggles() -> NSStackView {
        let entries = [("Complement strand", "arrow.left.arrow.right"),
                       ("Translation of open reading frames (green forward, red reverse)", "textformat"),
                       ("Restriction sites", "scissors"), ("Features", "tag")]
        viewToggles = entries.enumerated().map { (i, entry) -> NSButton in
            let image = NSImage(systemSymbolName: entry.1, accessibilityDescription: entry.0)
                ?? NSImage(size: NSSize(width: 14, height: 14))
            let button = NSButton(image: image, target: self, action: #selector(viewToggleClicked(_:)))
            button.isBordered = false
            button.tag = i
            button.toolTip = entry.0
            button.setAccessibilityLabel(entry.0)
            return button
        }
        let stack = NSStackView(views: viewToggles)
        stack.spacing = 8
        stack.setContentCompressionResistancePriority(.required, for: .horizontal)
        refreshToggles()
        return stack
    }

    @objc private func viewToggleClicked(_ sender: NSButton) {
        switch sender.tag {
        case 0: sequenceView.showComplement.toggle()
        case 1: core.frames = core.frames == 0 ? 6 : 0
        case 2: core.mapShowsEnzymes.toggle()
        default:
            // Hide every feature type on the map, or show them all again.
            core.mapHiddenTypes = core.mapHiddenTypes.isEmpty ? Set(core.features.map { $0.type }) : []
        }
        refreshToggles()
    }

    /// Tints each toggle that is on; menus and forms change the same options.
    private func refreshToggles() {
        let on = [sequenceView.showComplement, core.frames != 0, core.mapShowsEnzymes, core.mapHiddenTypes.isEmpty]
        for (button, isOn) in zip(viewToggles, on) {
            button.contentTintColor = isOn ? .controlAccentColor : .tertiaryLabelColor
        }
    }

    private func buildToolbar() {
        let toolbar = NSToolbar(identifier: "RestrictoToolbar")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        window?.toolbar = toolbar
        window?.toolbarStyle = .unified
        searchField.placeholderString = "Find bases (both strands)"
        searchField.sendsWholeSearchString = true
        searchField.target = self
        searchField.action = #selector(findFromField(_:))
    }

    /// NSDocument calls this when its unsaved-changes state flips: a small red
    /// dot just left of the file name in the tab. (A tab's accessory view sits
    /// at its far edge, away from the centred name, so the dot goes in the title.)
    override func setDocumentEdited(_ dirtyFlag: Bool) {
        super.setDocumentEdited(dirtyFlag)
        updateTabTitle()
    }

    override func synchronizeWindowTitleWithDocumentName() {
        super.synchronizeWindowTitleWithDocumentName()
        updateTabTitle()
    }

    private func updateTabTitle() {
        guard let tab = window?.tab else { return }
        guard let doc = document as? NSDocument, doc.isDocumentEdited else {
            tab.attributedTitle = nil
            return
        }
        let font = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        let title = NSMutableAttributedString(string: "●  ", attributes: [
            .font: NSFont.systemFont(ofSize: 7), .foregroundColor: NSColor.systemRed, .baselineOffset: 1.5,
        ])
        title.append(NSAttributedString(string: doc.displayName, attributes: [.font: font]))
        tab.attributedTitle = title
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
    private let mapItem = NSToolbarItem.Identifier("map")
    private let layoutItem = NSToolbarItem.Identifier("mapLayout")
    private let inspectorToolbarItem = NSToolbarItem.Identifier("inspector")

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator, .flexibleSpace, mapItem, layoutItem, exportItem, searchItem,
         inspectorToolbarItem]
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator, .flexibleSpace, .space, mapItem, layoutItem, complementItem,
         exportItem, searchItem, inspectorToolbarItem]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        switch id {
        case searchItem:
            let item = NSSearchToolbarItem(itemIdentifier: id)
            item.searchField = searchField
            item.preferredWidthForSearchField = 240
            return item
        case inspectorToolbarItem:
            return button(id, label: "Inspector", help: "Show or hide the right panel",
                          symbol: "sidebar.right", action: #selector(toggleInspectorPanel(_:)))
        case mapItem:
            return button(id, label: "Map", help: "Show or hide the map",
                          symbol: "circle.circle", action: #selector(toggleMapHidden(_:)))
        case layoutItem:
            return button(id, label: "Layout", help: "Put the map beside or above the sequence",
                          symbol: "rectangle.split.1x2", action: #selector(toggleMapStacked(_:)))
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
        refreshToggles()
    }

    @objc func toggleGroupTens(_ sender: Any?) {
        core.groupTens.toggle()
    }

    /// Shows or hides the right-hand panel. Not named toggleInspector: from
    /// macOS 14 NSSplitViewController has one, which comes first in the
    /// responder chain and never retitles the menu item.
    @objc func toggleInspectorPanel(_ sender: Any?) {
        guard let item = inspectorItem else { return }
        item.animator().isCollapsed = !item.isCollapsed
    }

    @objc func chooseBrowserFolder(_ sender: Any?) {
        browser.chooseFolder(sender)
    }

    @objc func toggleMapHidden(_ sender: Any?) {
        mapHidden.toggle()
        UserDefaults.standard.set(mapHidden, forKey: Self.mapHiddenKey)
        applyMapLayout()
        window?.layoutIfNeeded()
        placeDivider()
    }

    @objc func toggleMapStacked(_ sender: Any?) {
        mapStacked.toggle()
        UserDefaults.standard.set(mapStacked, forKey: Self.mapStackedKey)
        applyMapLayout()
        window?.layoutIfNeeded()
        placeDivider()
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
        case #selector(toggleGroupTens(_:)):
            item.state = core.groupTens ? .on : .off
            return true
        case #selector(toggleMapHidden(_:)):
            item.title = mapHidden ? "Show Map" : "Hide Map"
            return true
        case #selector(toggleMapStacked(_:)):
            item.state = mapStacked ? .on : .off
            return true
        case #selector(toggleInspectorPanel(_:)):
            item.title = inspectorItem?.isCollapsed == true ? "Show Inspector" : "Hide Inspector"
            return true
        case #selector(setFrames(_:)):
            item.state = core.frames == item.tag ? .on : .off
            return true
        default:
            return true
        }
    }
}

/// The map/sequence split with a divider that is easy to see and grab: a
/// separator line with a small handle in the middle.
final class MapSplitView: NSSplitView {
    override var dividerThickness: CGFloat { 7 }

    override func drawDivider(in rect: NSRect) {
        NSColor.separatorColor.setFill()
        let line = isVertical
            ? NSRect(x: rect.midX - 0.5, y: rect.minY, width: 1, height: rect.height)
            : NSRect(x: rect.minX, y: rect.midY - 0.5, width: rect.width, height: 1)
        line.fill()
        let handle = isVertical
            ? NSRect(x: rect.midX - 2, y: rect.midY - 16, width: 4, height: 32)
            : NSRect(x: rect.midX - 16, y: rect.midY - 2, width: 32, height: 4)
        NSColor.tertiaryLabelColor.setFill()
        NSBezierPath(roundedRect: handle, xRadius: 2, yRadius: 2).fill()
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
