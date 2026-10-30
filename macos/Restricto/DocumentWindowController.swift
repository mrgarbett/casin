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
/// The status is Liquid Glass capsules floating over the middle (see
/// Glass.swift), which split in two while there is a position or selection
/// to show.
final class DocumentWindowController: NSWindowController, NSToolbarDelegate, NSMenuItemValidation {
    private let core: CoreDocument
    private let mapView: MapView
    private let sequenceView: SequenceView
    /// The sequence view's scroll view, inside the pane that moves between the
    /// middle and the panel with the find bar over its bottom.
    private let sequenceScroll = NSScrollView()
    private let sequencePane = NSView()
    private let browser = FolderBrowserController()
    private let inspector: InspectorController
    private var inspectorItem: NSSplitViewItem?
    private var detailItem: NSSplitViewItem?
    private var splitController: NSSplitViewController?
    private let status = NSTextField(labelWithString: "")
    /// View toggles beside the status: complement strand, translation, and
    /// enzymes and features on the map.
    private var viewToggles: [NSButton] = []
    private let searchField = NSSearchField()
    /// The find bar (⌘F) at the bottom of the sequence, hidden until asked for,
    /// and its bottom edge, raised over the floating status where that covers
    /// the sequence.
    private var findBar: NSView?
    private var findBarBottom: NSLayoutConstraint?
    private var findBarCentre: NSLayoutConstraint?
    /// The status's second capsule: the caret or selection, and the
    /// base under the mouse. Hidden while there is nothing to show.
    private var infoCapsule: NSView?
    /// The base under the mouse in the sequence or on the map, or -1.
    private var hoverBase = -1
    /// A message (from Find) shown in the status until the selection changes.
    private var statusMessage: String?
    /// The document name, edited dot and subtitle, centred in the toolbar.
    private let titleView = DocumentTitleView()
    /// Keeps the glass status inside the sequence pane. A constant, updated on
    /// resize, so a longer status can't push the map/sequence divider.
    private var pillMaxWidth: NSLayoutConstraint?
    /// Map | sequence, or map over sequence (View > Stack Map Above Sequence).
    private let split = MapSplitView()
    /// The floating status capsule, and its bottom edge: under the
    /// map when the map is stacked over the sequence, else the window's bottom.
    private var statusPill: NSView?
    private var pillBottom: NSLayoutConstraint?
    private var detailRoot: NSView?
    /// Room the panel's pill takes along the middle's right edge.
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
        core.observe { [weak self] what in
            if what & Change.selection != 0 { self?.statusMessage = nil }
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
            DispatchQueue.main.async { self?.updatePillMaxWidth() }
        }

        scroll.translatesAutoresizingMaskIntoConstraints = false
        sequencePane.addSubview(scroll)
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: sequencePane.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: sequencePane.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: sequencePane.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: sequencePane.bottomAnchor),
        ])
        let pane = sequencePane
        split.isVertical = true
        split.addArrangedSubview(mapView)
        split.addArrangedSubview(pane)
        split.setHoldingPriority(.defaultLow, forSubviewAt: 0)
        split.setHoldingPriority(.defaultLow - 1, forSubviewAt: 1)
        minWidths = [mapView.widthAnchor.constraint(greaterThanOrEqualToConstant: 260),
                     pane.widthAnchor.constraint(greaterThanOrEqualToConstant: 320)]
        minHeights = [mapView.heightAnchor.constraint(greaterThanOrEqualToConstant: 180),
                      pane.heightAnchor.constraint(greaterThanOrEqualToConstant: 180)]
        // The base under the mouse goes in the status.
        let hover: (Int) -> Void = { [weak self] base in
            guard let self = self, base != self.hoverBase else { return }
            self.hoverBase = base
            self.updateStatus()
        }
        sequenceView.onHoverBase = hover
        mapView.onHoverBase = hover

        status.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let detail = NSViewController()
        let root = DropView(frame: NSRect(x: 0, y: 0, width: 1040, height: 820))
        detail.view = root
        detailRoot = root
        // Content fills the pane below the toolbar; the status floats
        // over the bottom of the sequence (or of the map).
        let pill = floatingStatus()
        let content = split
        for v in [content, pill] {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        scroll.automaticallyAdjustsContentInsets = false
        // Centred in the middle, so it sits centred below the map.
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            content.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor),
            content.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])
        // Shifted as the map is, which leaves room for the panel's pill on
        // the right. The edges stay inside the visible middle: on a narrow
        // middle the status text truncates instead of the pill running
        // under the sidebar or the panel pill.
        // The max width (updatePillMaxWidth) is what keeps it inside; these
        // guards stay below the split view's holding priority, so the pill
        // never raises the middle's minimum width, and the window's with it.
        let centre = pill.centerXAnchor.constraint(equalTo: root.safeAreaLayoutGuide.centerXAnchor,
                                                   constant: -Self.panelPillRoom / 2)
        centre.priority = .defaultHigh
        let leading = pill.leadingAnchor.constraint(greaterThanOrEqualTo: root.safeAreaLayoutGuide.leadingAnchor,
                                                    constant: 8)
        let trailing = pill.trailingAnchor.constraint(lessThanOrEqualTo: root.safeAreaLayoutGuide.trailingAnchor,
                                                      constant: -8 - Self.panelPillRoom)
        leading.priority = .defaultLow - 1
        trailing.priority = .defaultLow - 1
        NSLayoutConstraint.activate([centre, leading, trailing])
        root.postsFrameChangedNotifications = true
        NotificationCenter.default.addObserver(forName: NSView.frameDidChangeNotification, object: root,
                                               queue: .main) { [weak self] _ in
            DispatchQueue.main.async { self?.updatePillMaxWidth() }
        }
        statusPill = pill
        pillMaxWidth = pill.widthAnchor.constraint(lessThanOrEqualToConstant: 400)
        pillMaxWidth?.isActive = true

        // The panel's pill floats at the top right of the middle: at the
        // window's edge while the panel is hidden, and it slides left with the
        // panel's edge as the panel opens.
        let panelPill = inspector.pill
        panelPill.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(panelPill)
        NSLayoutConstraint.activate([
            panelPill.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor, constant: 10),
            panelPill.trailingAnchor.constraint(equalTo: root.safeAreaLayoutGuide.trailingAnchor, constant: -8),
        ])
        mapView.trailingInset = Self.panelPillRoom
        buildFindBar(in: sequencePane)
        // Double-clicking a feature (map, sequence or panel list) edits it.
        let edit: (Int) -> Void = { [weak self] index in self?.editFeature(at: index) }
        inspector.onEditFeature = edit
        sequenceView.onEditFeature = edit
        sequenceView.onAddPrimer = { [weak self] in self?.addPrimer(nil) }
        inspector.onPcr = { [weak self] in self?.pcrProduct(nil) }
        mapView.onEditFeature = edit
        inspector.onPick = { [weak self] _, wasShowing in
            guard let item = self?.inspectorItem else { return }
            // A hidden panel opens on the pane picked; picking the pane already
            // showing hides the panel again.
            if item.isCollapsed { item.animator().isCollapsed = false }
            else if wasShowing { item.animator().isCollapsed = true }
        }

        browser.fallbackFolder = { [weak self] in
            (self?.document as? NSDocument)?.fileURL?.deletingLastPathComponent()
        }
        let sidebarItem = NSSplitViewItem(sidebarWithViewController: browser)
        // Wide enough for the window buttons, the sidebar button and the
        // undo/redo/save/export pill above it.
        sidebarItem.minimumThickness = 270
        sidebarItem.maximumThickness = 400
        sidebarItem.canCollapse = true
        let detailItem = NSSplitViewItem(viewController: detail)
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
        // The panel toggle's tint follows the panel, however it was shown or hidden.
        NotificationCenter.default.addObserver(forName: NSSplitView.didResizeSubviewsNotification,
                                               object: splitController.splitView, queue: .main) { [weak self] _ in
            DispatchQueue.main.async { self?.refreshToolbarToggles() }
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
        // One document: no tab bar (the title shows its name and unsaved dot).
        // Earlier builds kept the bar up, and AppKit remembers that choice, so
        // take it down once; with two or more tabs AppKit shows it again.
        if let group = window.tabGroup, group.isTabBarVisible, group.windows.count <= 1 { window.toggleTabBar(nil) }
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
            if sequencePane.superview === split {
                split.removeArrangedSubview(sequencePane)
                sequencePane.removeFromSuperview()
            }
            inspector.hostSequence(sequencePane)
            NSLayoutConstraint.activate([minWidths[0]])
        } else {
            inspector.hostSequence(nil)
            if sequencePane.superview !== split {
                split.addArrangedSubview(sequencePane)
                split.setHoldingPriority(.defaultLow - 1, forSubviewAt: 1)
            }
            let mins = mapStacked ? minHeights : minWidths
            NSLayoutConstraint.activate(mapHidden ? [mins[1]] : mins)
        }
        // The floating status sits under the map when the map is stacked over
        // the sequence, else at the bottom, where the sequence scrolls clear of it.
        let statusUnderMap = mapStacked && !mapHidden
        let statusOverSequence = !sequenceInPanel && !statusUnderMap
        sequenceScroll.contentInsets = NSEdgeInsets(top: 0, left: 0, bottom: statusOverSequence ? 52 : 0, right: 0)
        // Under the map, the map's labels keep clear of it.
        mapView.bottomInset = statusUnderMap ? 46 : 0
        if let pill = statusPill, let root = detailRoot {
            pillBottom?.isActive = false
            pillBottom = statusUnderMap
                ? pill.bottomAnchor.constraint(equalTo: mapView.bottomAnchor, constant: -10)
                : pill.bottomAnchor.constraint(equalTo: root.safeAreaLayoutGuide.bottomAnchor, constant: -12)
            pillBottom?.isActive = true
        }
        // The find bar sits at the bottom of the sequence, above the status
        // where the status floats over it.
        findBarBottom?.constant = statusOverSequence ? -52 : -12
        // In the middle without the map, the sequence leaves room for the pill.
        sequenceView.trailingInset = !sequenceInPanel && mapHidden ? Self.panelPillRoom : 0
        findBarCentre?.constant = -sequenceView.trailingInset / 2
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

    /// Opens the right panel on Primers with a new primer's inputs.
    func beginAddingPrimer(bases: String) {
        if let item = inspectorItem, item.isCollapsed { item.animator().isCollapsed = false }
        inspector.beginAddingPrimer(bases: bases)
    }

    /// The widest the status pill may be: the middle less the panel pill's room
    /// and a margin. Called after layout, never from inside it, and the
    /// constant changes only when the width really did.
    private func updatePillMaxWidth() {
        guard let limit = pillMaxWidth, let root = detailRoot else { return }
        let width = max(0, root.safeAreaRect.width - Self.panelPillRoom - 16)
        if abs(limit.constant - width) >= 1 { limit.constant = width }
    }

    /// The status as glass capsules: the view toggles
    /// in one, and beside it, while there is something to show, a second with
    /// the caret or selection (positions, length, GC, Tm) and the base under
    /// the mouse. The two sit in a glass container, so the second splits off
    /// the first when it appears and merges back into it when it goes.
    private func floatingStatus() -> NSView {
        let togglesInner = NSView()
        let toggles = makeToggles()
        toggles.translatesAutoresizingMaskIntoConstraints = false
        togglesInner.addSubview(toggles)
        NSLayoutConstraint.activate([
            toggles.leadingAnchor.constraint(equalTo: togglesInner.leadingAnchor, constant: 12),
            toggles.trailingAnchor.constraint(equalTo: togglesInner.trailingAnchor, constant: -12),
            toggles.centerYAnchor.constraint(equalTo: togglesInner.centerYAnchor),
        ])
        let infoInner = NSView()
        status.translatesAutoresizingMaskIntoConstraints = false
        infoInner.addSubview(status)
        NSLayoutConstraint.activate([
            status.leadingAnchor.constraint(equalTo: infoInner.leadingAnchor, constant: 14),
            status.trailingAnchor.constraint(equalTo: infoInner.trailingAnchor, constant: -14),
            status.centerYAnchor.constraint(equalTo: infoInner.centerYAnchor),
        ])
        let togglesCapsule = Glass.capsule(around: togglesInner, height: 30)
        let info = Glass.capsule(around: infoInner, height: 30)
        info.isHidden = true
        infoCapsule = info
        let row = NSStackView(views: [togglesCapsule, info])
        row.orientation = .horizontal
        // Two pills apart; while the second slides out of the first, the glass
        // stretches between them until they are further apart than `spacing`.
        row.spacing = 10
        togglesCapsule.setContentCompressionResistancePriority(.required, for: .horizontal)
        // The capsule hugs the text, so the text must hold it open; the pill's
        // max-width constraint still truncates it in a narrow window.
        status.setContentCompressionResistancePriority(.defaultHigh, for: .horizontal)
        // Secondary grey washes out over the sequence text behind the glass.
        status.textColor = .labelColor
        return Glass.mergingContainer(around: row, spacing: 8)
    }

    /// Small icon toggles for the status pill.
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
        refreshToolbarToggles()
    }

    private func buildToolbar() {
        let toolbar = NSToolbar(identifier: "RestrictoToolbar")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        // The title is drawn by the centred title item, with its dot.
        toolbar.centeredItemIdentifiers = [titleItem]
        window?.titleVisibility = .hidden
        window?.toolbar = toolbar
        window?.toolbarStyle = .unified
        searchField.placeholderString = "Find bases (both strands)"
        searchField.sendsWholeSearchString = true
        searchField.target = self
        searchField.action = #selector(findFromField(_:))
    }

    /// NSDocument calls this when its unsaved-changes state flips: a small red
    /// dot beside the centred title, and just left of the file name in the tab
    /// (shown with two or more documents). A tab's accessory view sits at its
    /// far edge, away from the centred name, so that dot goes in the tab title.
    override func setDocumentEdited(_ dirtyFlag: Bool) {
        super.setDocumentEdited(dirtyFlag)
        updateTabTitle()
    }

    override func synchronizeWindowTitleWithDocumentName() {
        super.synchronizeWindowTitleWithDocumentName()
        updateTabTitle()
    }

    private func updateTabTitle() {
        updateTitle()
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
        updateStatus()
        updateTitle()
    }

    /// The status: a Find message, or the caret or selection and the base under
    /// the mouse. The second capsule shows only while there is text.
    private func updateStatus() {
        var parts: [String] = []
        if let message = statusMessage {
            parts.append(message)
        } else {
            let selection = core.selectionText
            if !selection.isEmpty { parts.append(selection) }
            if hoverBase >= 0 && hoverBase < core.length { parts.append("Base \((hoverBase + 1).formatted())") }
        }
        status.stringValue = parts.joined(separator: "  ·  ")
        guard let info = infoCapsule else { return }
        let show = !parts.isEmpty
        guard info.isHidden == show else { return }
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.25
            context.allowsImplicitAnimation = true
            info.isHidden = !show
            info.superview?.layoutSubtreeIfNeeded()
        }
    }

    /// The centred title: the file name, a red dot while there are unsaved
    /// changes, and under it the sequence name (when it differs), length and
    /// topology.
    private func updateTitle() {
        let doc = document as? NSDocument
        let edited = (doc?.isDocumentEdited ?? false) || core.isModified
        let subtitle: String
        if core.isEmpty {
            subtitle = "Open a file from the sidebar, or drop one here"
        } else {
            var parts = ["\(core.length.formatted()) bp", core.isCircular ? "circular" : "linear"]
            let fileName = doc?.fileURL?.deletingPathExtension().lastPathComponent
            if !core.name.isEmpty && core.name != fileName { parts.insert(core.name, at: 0) }
            subtitle = parts.joined(separator: " · ")
        }
        titleView.update(name: doc?.displayName ?? (core.name.isEmpty ? "Untitled" : core.name),
                         subtitle: subtitle, edited: edited)
    }

    // MARK: Toolbar

    /// Undo, redo, save and export, as one pill beside the sidebar button.
    private let actionsItem = NSToolbarItem.Identifier("actions")
    private let titleItem = NSToolbarItem.Identifier("title")
    private let complementItem = NSToolbarItem.Identifier("complement")
    private let mapItem = NSToolbarItem.Identifier("map")
    private let layoutItem = NSToolbarItem.Identifier("mapLayout")
    private let inspectorToolbarItem = NSToolbarItem.Identifier("inspector")

    /// Toggles in the toolbar, with their symbols: each turns accent blue while on.
    private var toolbarToggles: [NSToolbarItem.Identifier: (item: NSToolbarItem, symbol: String)] = [:]
    private var toolbarToggleShown: [NSToolbarItem.Identifier: Bool] = [:]

    /// The Export menu: the map as an image, the sequence as GenBank or FASTA.
    private lazy var exportMenu: NSMenu = {
        let menu = NSMenu()
        for (title, action) in [("Map Image (PNG or PDF)…", #selector(exportMap(_:))),
                                ("GenBank…", #selector(saveAsGenBank(_:))),
                                ("FASTA…", #selector(exportFasta(_:)))] {
            let entry = NSMenuItem(title: title, action: action, keyEquivalent: "")
            entry.target = self
            menu.addItem(entry)
        }
        return menu
    }()

    // The actions pill sits just past the sidebar button: first in the
    // toolbar's main part, which starts at the sidebar's edge. (Before the
    // tracking separator it shares the sidebar's width with the window buttons
    // and never fits, so AppKit moves it into the overflow menu.) The title
    // is in the centre, the view toggles at the right. Find opens on ⌘F.
    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator, actionsItem, .flexibleSpace, titleItem, .flexibleSpace,
         mapItem, layoutItem, inspectorToolbarItem]
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        [.toggleSidebar, .sidebarTrackingSeparator, actionsItem, titleItem, .flexibleSpace, .space,
         mapItem, layoutItem, complementItem, inspectorToolbarItem]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        switch id {
        case titleItem:
            let item = NSToolbarItem(itemIdentifier: id)
            item.view = titleView
            item.label = "Title"
            item.isBordered = false
            updateTitle()
            return item
        case actionsItem:
            // Plain buttons in one group, so they share a capsule the way the
            // toggles on the right do, without a segmented control's dividers.
            let entries: [(String, String, String, Selector)] = [
                ("Undo", "arrow.uturn.backward", "Undo the last edit (⌘Z)", #selector(undoEdit(_:))),
                ("Redo", "arrow.uturn.forward", "Redo (⇧⌘Z)", #selector(redoEdit(_:))),
                ("Save", "square.and.arrow.down", "Save (⌘S)", #selector(saveFromToolbar(_:))),
                ("Export", "square.and.arrow.up", "Export the map, or the sequence as GenBank or FASTA",
                 #selector(exportFromToolbar(_:))),
            ]
            let group = NSToolbarItemGroup(itemIdentifier: id)
            group.subitems = entries.map {
                button(NSToolbarItem.Identifier("actions.\($0.0)"), label: $0.0, help: $0.2, symbol: $0.1, action: $0.3)
            }
            group.label = "Actions"
            group.paletteLabel = "Undo, Redo, Save, Export"
            return group
        case inspectorToolbarItem:
            return toggle(id, label: "Panel", help: "Show or hide the right panel",
                          symbol: "sidebar.right", action: #selector(toggleInspectorPanel(_:)))
        case mapItem:
            return toggle(id, label: "Map", help: "Show or hide the map",
                          symbol: "circle.circle", action: #selector(toggleMapHidden(_:)))
        case layoutItem:
            return toggle(id, label: "Stack", help: "Stack the map above the sequence, or put it beside",
                          symbol: "rectangle.split.1x2", action: #selector(toggleMapStacked(_:)))
        case complementItem:
            return toggle(id, label: "Complement", help: "Show or hide the complement strand",
                          symbol: "arrow.left.arrow.right", action: #selector(toggleComplementStrand(_:)))
        default:
            return nil
        }
    }

    @objc private func saveFromToolbar(_ sender: Any?) { (document as? NSDocument)?.save(sender) }

    @objc private func exportFromToolbar(_ sender: Any?) {
        exportMenu.popUp(positioning: nil, at: NSEvent.mouseLocation, in: nil)
    }

    /// A toolbar button for something that is on or off; refreshToolbarToggles
    /// tints it.
    private func toggle(_ id: NSToolbarItem.Identifier, label: String, help: String, symbol: String,
                        action: Selector) -> NSToolbarItem {
        let item = button(id, label: label, help: help, symbol: symbol, action: action)
        toolbarToggles[id] = (item, symbol)
        refreshToolbarToggles()
        return item
    }

    /// Accent blue for a toolbar toggle that is on: the map shown, the map
    /// stacked, the panel shown, the complement strand shown.
    private func refreshToolbarToggles() {
        let on: [NSToolbarItem.Identifier: Bool] = [
            mapItem: !mapHidden, layoutItem: mapStacked,
            inspectorToolbarItem: !(inspectorItem?.isCollapsed ?? true) && !inspectorIsZeroWidth,
            complementItem: sequenceView.showComplement,
        ]
        for (id, entry) in toolbarToggles {
            let isOn = on[id] ?? false
            guard toolbarToggleShown[id] != isOn else { continue }
            toolbarToggleShown[id] = isOn
            let base = NSImage(systemSymbolName: entry.symbol, accessibilityDescription: entry.item.label)
            entry.item.image = isOn ? base?.withSymbolConfiguration(.init(paletteColors: [.controlAccentColor])) : base
        }
    }

    /// A plain bordered toolbar button; the system groups these into glass.
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
            statusMessage = "Find takes DNA bases (IUPAC codes such as N and R work too)"
        } else if result.match == 0 {
            NSSound.beep()
            statusMessage = "No match for \(query.uppercased()) on either strand"
        }
        updateStatus()
    }

    /// ⌘F: shows the sequence (opening the panel on it when the map is beside
    /// it), opens the find bar at the sequence's bottom and puts the cursor
    /// in it; again (or Esc) closes it. Not performFindPanelAction: while the
    /// find field is being edited its field editor, an NSTextView, has that
    /// action too and takes ⌘F first, so the bar would never close.
    @objc func toggleFindBar(_ sender: Any?) {
        guard let bar = findBar else { return }
        let editing = searchField.currentEditor() != nil
        if !bar.isHidden && editing {
            closeFindBar()
        } else {
            revealSequence()
            bar.isHidden = false
            window?.makeFirstResponder(searchField)
        }
    }

    /// Makes the sequence visible: with the map beside it, opens the right
    /// panel on its Sequence pane.
    func revealSequence() {
        guard sequenceInPanel else { return }
        if let item = inspectorItem, item.isCollapsed { item.animator().isCollapsed = false }
        inspector.select(.sequence)
    }

    private func closeFindBar() {
        findBar?.isHidden = true
        window?.makeFirstResponder(sequenceView)
    }

    /// The find field in a capsule, centred at the bottom of the sequence
    /// (above the status where that floats over it); hidden until ⌘F.
    private func buildFindBar(in pane: NSView) {
        searchField.translatesAutoresizingMaskIntoConstraints = false
        let width = searchField.widthAnchor.constraint(equalToConstant: 260)
        width.priority = .defaultHigh
        width.isActive = true
        searchField.delegate = self
        let inner = NSView()
        inner.addSubview(searchField)
        NSLayoutConstraint.activate([
            searchField.leadingAnchor.constraint(equalTo: inner.leadingAnchor, constant: 8),
            searchField.trailingAnchor.constraint(equalTo: inner.trailingAnchor, constant: -8),
            searchField.centerYAnchor.constraint(equalTo: inner.centerYAnchor),
        ])
        let bar = Glass.capsule(around: inner, height: 38)
        bar.translatesAutoresizingMaskIntoConstraints = false
        bar.isHidden = true
        pane.addSubview(bar)
        // applyMapLayout sets both constants for the layout.
        let bottom = bar.bottomAnchor.constraint(equalTo: pane.bottomAnchor, constant: -12)
        let centre = bar.centerXAnchor.constraint(equalTo: pane.centerXAnchor)
        // Never wider than the sequence; the field gives way first.
        let leading = bar.leadingAnchor.constraint(greaterThanOrEqualTo: pane.leadingAnchor, constant: 8)
        NSLayoutConstraint.activate([bottom, centre, leading])
        findBarBottom = bottom
        findBarCentre = centre
        findBar = bar
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
        if inspectorIsZeroWidth {
            showZeroWidthInspector()
            return
        }
        item.animator().isCollapsed = !item.isCollapsed
    }

    /// The split view can restore the panel open but 0 points wide: not
    /// collapsed, so the menu says Hide, yet nothing shows.
    private var inspectorIsZeroWidth: Bool {
        guard let item = inspectorItem, !item.isCollapsed else { return false }
        return item.viewController.view.frame.width < 1
    }

    private func showZeroWidthInspector() {
        guard let item = inspectorItem, let split = splitController?.splitView,
              split.arrangedSubviews.count == 3 else { return }
        let width = max(item.minimumThickness, 300)
        split.setPosition(split.bounds.width - width - split.dividerThickness, ofDividerAt: 1)
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
        refreshToolbarToggles()
    }

    @objc func toggleMapStacked(_ sender: Any?) {
        mapStacked.toggle()
        UserDefaults.standard.set(mapStacked, forKey: Self.mapStackedKey)
        applyMapLayout()
        window?.layoutIfNeeded()
        placeDivider()
        refreshToolbarToggles()
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
            item.title = inspectorItem?.isCollapsed == true || inspectorIsZeroWidth ? "Show Inspector" : "Hide Inspector"
            return true
        case #selector(setFrames(_:)):
            item.state = core.frames == item.tag ? .on : .off
            return true
        default:
            return true
        }
    }
}

extension DocumentWindowController: NSSearchFieldDelegate {
    /// Esc in the find field closes the find bar.
    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        guard control === searchField, selector == #selector(NSResponder.cancelOperation(_:)) else { return false }
        closeFindBar()
        return true
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

/// The window's title, centred in the toolbar: the file name with a red dot
/// while there are unsaved changes, and a smaller line under it.
final class DocumentTitleView: NSView {
    private let dot = UnsavedDot(diameter: 7)
    private let name = NSTextField(labelWithString: "")
    private let subtitle = NSTextField(labelWithString: "")

    init() {
        super.init(frame: NSRect(x: 0, y: 0, width: 240, height: 30))
        name.font = .systemFont(ofSize: NSFont.systemFontSize, weight: .semibold)
        name.lineBreakMode = .byTruncatingMiddle
        name.alignment = .center
        subtitle.font = .systemFont(ofSize: NSFont.smallSystemFontSize - 1)
        subtitle.textColor = .secondaryLabelColor
        subtitle.lineBreakMode = .byTruncatingTail
        subtitle.alignment = .center
        for label in [name, subtitle] {
            label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        }
        let top = NSStackView(views: [name, dot])
        top.spacing = 5
        top.alignment = .centerY
        let stack = NSStackView(views: [top, subtitle])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 0
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(greaterThanOrEqualTo: leadingAnchor),
            stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor),
            stack.centerXAnchor.constraint(equalTo: centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            widthAnchor.constraint(lessThanOrEqualToConstant: 420),
            widthAnchor.constraint(greaterThanOrEqualToConstant: 160),
            heightAnchor.constraint(equalToConstant: 30),
        ])
        dot.isHidden = true
        setAccessibilityElement(true)
        setAccessibilityRole(.staticText)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    func update(name: String, subtitle: String, edited: Bool) {
        self.name.stringValue = name
        self.subtitle.stringValue = subtitle
        self.subtitle.isHidden = subtitle.isEmpty
        dot.isHidden = !edited
        toolTip = edited ? "\(name) (unsaved changes)" : name
        setAccessibilityLabel(toolTip)
        invalidateIntrinsicContentSize()
    }

    /// As wide as the name or subtitle needs, within the constraints above.
    override var intrinsicContentSize: NSSize {
        let width = max(name.intrinsicContentSize.width + 14, subtitle.intrinsicContentSize.width) + 8
        return NSSize(width: min(420, max(160, width)), height: 30)
    }
}
