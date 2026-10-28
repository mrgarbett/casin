// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import RestrictoCore

/// The window's right-hand panel: the sequence (when the map sits beside it),
/// Enzymes, Features or Primers, picked from a vertical pill. The window floats
/// the pill at the left of the middle, where it stays when the panel is hidden. Vibrant on Ventura, glass on macOS 26.
final class InspectorController: NSViewController, NSTableViewDataSource, NSTableViewDelegate {
    enum Pane: String, CaseIterable {
        case sequence, enzymes, features, primers
    }

    private let document: CoreDocument
    private let tabs = NSTabView()
    /// The pill's buttons, one per pane in Pane order.
    private var pillButtons: [NSButton] = []
    /// The vertical pill; the window places it.
    private(set) lazy var pill: NSView = buildPill()
    /// Called after a pill button picks a pane, with whether it was already showing.
    var onPick: ((Pane, Bool) -> Void)?
    /// Holds the window's sequence view while the panel shows it.
    private let sequenceHolder = NSView()
    private let featureTable = NSTableView()
    private let enzymeTable = NSTableView()
    private let primerTable = NSTableView()
    private let filter = NSPopUpButton(frame: .zero, pullsDown: false)
    private let enzymeSearch = NSSearchField()
    private let onMapOnly = NSButton(checkboxWithTitle: "Only enzymes on the map", target: nil, action: nil)
    private var features: [FeatureInfo] = []
    /// Every enzyme that cuts, and the ones the search and toggle leave in the list.
    private var cutters: [EnzymeInfo] = []
    private var enzymes: [EnzymeInfo] = []
    private var primers: [PrimerInfo] = []
    private var syncing = false
    /// The table pane new windows open on, and whether the list keeps only enzymes on the map.
    private static let paneKey = "InspectorPane"
    private static let onMapOnlyKey = "EnzymesOnMapOnly"
    private(set) var pane: Pane = .enzymes
    /// The pane last picked other than the sequence.
    private var tablePane: Pane = Pane(rawValue: UserDefaults.standard.string(forKey: InspectorController.paneKey) ?? "")
        ?? .enzymes
    var hostsSequence: Bool { !sequenceHolder.subviews.isEmpty }

    init(document: CoreDocument) {
        self.document = document
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func loadView() {
        tabs.tabViewType = .noTabsNoBorder
        tabs.drawsBackground = false
        let sequenceItem = NSTabViewItem(identifier: Pane.sequence.rawValue)
        sequenceItem.label = "Sequence"
        sequenceItem.view = sequenceHolder
        tabs.addTabViewItem(sequenceItem)
        buildEnzymesTab()
        buildFeaturesTab()
        buildPrimersTab()

        let root = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 600))
        tabs.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(tabs)
        NSLayoutConstraint.activate([
            tabs.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor, constant: 4),
            tabs.leadingAnchor.constraint(equalTo: root.safeAreaLayoutGuide.leadingAnchor),
            tabs.trailingAnchor.constraint(equalTo: root.safeAreaLayoutGuide.trailingAnchor),
            tabs.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])
        view = root

        document.observe { [weak self] what in
            guard let self = self else { return }
            if what & Change.sequence != 0 { self.reloadFeatures(); self.reloadPrimers() }
            if what & (Change.sequence | Change.enzymes) != 0 { self.reloadEnzymes() }
            if what & Change.selection != 0 { self.syncSelection() }
        }
        reloadFeatures()
        reloadEnzymes()
        reloadPrimers()
        select(hostsSequence ? .sequence : tablePane)
    }

    /// Shows `view` (the window's sequence) as the panel's first pane, or
    /// gives it back (nil). The pill offers Sequence only while it is here.
    func hostSequence(_ view: NSView?) {
        sequenceHolder.subviews.forEach { $0.removeFromSuperview() }
        if let view = view {
            view.translatesAutoresizingMaskIntoConstraints = false
            sequenceHolder.addSubview(view)
            NSLayoutConstraint.activate([
                view.leadingAnchor.constraint(equalTo: sequenceHolder.leadingAnchor),
                view.trailingAnchor.constraint(equalTo: sequenceHolder.trailingAnchor),
                view.topAnchor.constraint(equalTo: sequenceHolder.topAnchor),
                view.bottomAnchor.constraint(equalTo: sequenceHolder.bottomAnchor),
            ])
        }
        pillButtons.first?.isHidden = view == nil
        guard isViewLoaded else { return }
        select(view != nil ? .sequence : tablePane)
    }

    func select(_ pane: Pane) {
        let pane = pane == .sequence && !hostsSequence ? tablePane : pane
        self.pane = pane
        tabs.selectTabViewItem(withIdentifier: pane.rawValue)
        for (button, p) in zip(pillButtons, Pane.allCases) {
            button.contentTintColor = p == pane ? .controlAccentColor : .secondaryLabelColor
        }
    }

    override func viewDidLayout() {
        super.viewDidLayout()
        // The columns start wider than a narrow panel; fit them to it.
        featureTable.sizeToFit()
        enzymeTable.sizeToFit()
        primerTable.sizeToFit()
    }

    /// The vertical pill: one icon button per tab, the current one tinted.
    private func buildPill() -> NSView {
        let entries = [("Sequence", "text.alignleft"), ("Enzymes", "scissors"), ("Features", "tag"),
                       ("Primers", "arrow.right")]
        pillButtons = entries.enumerated().map { (i, entry) -> NSButton in
            let image = NSImage(systemSymbolName: entry.1, accessibilityDescription: entry.0)
                ?? NSImage(size: NSSize(width: 16, height: 16))
            let button = NSButton(image: image, target: self, action: #selector(pillClicked(_:)))
            button.isBordered = false
            button.tag = i
            button.toolTip = entry.0
            button.setAccessibilityLabel(entry.0)
            button.imageScaling = .scaleProportionallyUpOrDown
            button.translatesAutoresizingMaskIntoConstraints = false
            button.widthAnchor.constraint(equalToConstant: 28).isActive = true
            button.heightAnchor.constraint(equalToConstant: 28).isActive = true
            return button
        }
        pillButtons.first?.isHidden = !hostsSequence
        let stack = NSStackView(views: pillButtons)
        stack.orientation = .vertical
        stack.spacing = 4
        stack.edgeInsets = NSEdgeInsets(top: 6, left: 4, bottom: 6, right: 4)
        return Glass.verticalCapsule(around: stack, width: 36)
    }

    @objc private func pillClicked(_ sender: NSButton) {
        let pane = Pane.allCases[sender.tag]
        let wasShowing = pane == self.pane
        if pane != .sequence {
            tablePane = pane
            UserDefaults.standard.set(pane.rawValue, forKey: Self.paneKey)
        }
        select(pane)
        onPick?(pane, wasShowing)
    }

    private func makeTable(_ table: NSTableView, columns: [(String, String, CGFloat)]) -> NSScrollView {
        for (id, title, width) in columns {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
            column.title = title
            column.width = width
            column.minWidth = min(width, 40)
            // Checkbox and count columns keep their width; the others share the rest.
            column.resizingMask = width <= 40 ? [] : .autoresizingMask
            table.addTableColumn(column)
        }
        // Fit the columns to the panel instead of overflowing it.
        table.columnAutoresizingStyle = .uniformColumnAutoresizingStyle
        table.style = .sourceList
        table.usesAlternatingRowBackgroundColors = false
        table.allowsMultipleSelection = false
        table.rowHeight = 24
        table.dataSource = self
        table.delegate = self
        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        return scroll
    }

    private func buildPrimersTab() {
        let scroll = makeTable(primerTable, columns: [("name", "Primer", 120), ("tm", "Tm", 56),
                                                      ("location", "Binds", 96)])
        let item = NSTabViewItem(identifier: Pane.primers.rawValue)
        item.label = "Primers"
        item.view = scroll
        tabs.addTabViewItem(item)
    }

    private func buildFeaturesTab() {
        let scroll = makeTable(featureTable, columns: [("name", "Feature", 150), ("location", "Location", 110)])
        let item = NSTabViewItem(identifier: Pane.features.rawValue)
        item.label = "Features"
        item.view = scroll
        tabs.addTabViewItem(item)
    }

    private func buildEnzymesTab() {
        filter.addItems(withTitles: ["Unique cutters", "Up to 2 cuts", "Up to 3 cuts", "All cutters"])
        filter.target = self
        filter.action = #selector(filterChanged(_:))
        enzymeSearch.placeholderString = "Find enzyme"
        enzymeSearch.controlSize = .small
        enzymeSearch.target = self
        enzymeSearch.action = #selector(enzymeSearchChanged(_:))
        onMapOnly.title = "On map only"
        onMapOnly.controlSize = .small
        onMapOnly.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        onMapOnly.toolTip = "Hide enzymes the cut filter leaves out or that are unticked"
        onMapOnly.target = self
        onMapOnly.action = #selector(onMapOnlyChanged(_:))
        onMapOnly.state = UserDefaults.standard.bool(forKey: Self.onMapOnlyKey) ? .on : .off
        let scroll = makeTable(enzymeTable, columns: [("show", "", 24), ("name", "Enzyme", 80),
                                                       ("site", "Site", 100), ("cuts", "Cuts", 40)])
        let credit = NSTextField(labelWithString: "Enzyme data: REBASE, rebase.neb.com")
        credit.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        credit.textColor = .secondaryLabelColor
        // The cut filter takes the row's spare width; the switch keeps its own.
        let filterRow = NSStackView(views: [filter, onMapOnly])
        filterRow.spacing = 8
        filter.setContentHuggingPriority(.defaultLow - 1, for: .horizontal)
        filter.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        onMapOnly.setContentHuggingPriority(.required, for: .horizontal)
        let stack = NSStackView(views: [enzymeSearch, filterRow, scroll, credit])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.distribution = .fill
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 8, bottom: 8, right: 8)
        // The table has no height of its own; let it take the spare space.
        scroll.setContentHuggingPriority(.defaultLow - 1, for: .vertical)
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -16).isActive = true
        enzymeSearch.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -16).isActive = true
        filterRow.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -16).isActive = true
        // NSTabView shrinks a stack view item to its fitting size, so give the
        // tab a plain container and pin the stack inside it.
        let container = NSView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            stack.topAnchor.constraint(equalTo: container.topAnchor),
            stack.bottomAnchor.constraint(equalTo: container.bottomAnchor),
        ])
        let item = NSTabViewItem(identifier: Pane.enzymes.rawValue)
        item.label = "Enzymes"
        item.view = container
        tabs.addTabViewItem(item)
    }

    private func reloadFeatures() {
        features = document.features.filter { $0.drawn }
        featureTable.reloadData()
    }

    private func reloadPrimers() {
        primers = document.primers
        primerTable.reloadData()
    }

    private func reloadEnzymes() {
        cutters = document.enzymes.filter { $0.cuts > 0 }
            .sorted { $0.name.lowercased() < $1.name.lowercased() }
        let index = [1: 0, 2: 1, 3: 2, 0: 3][document.maxCuts] ?? 0
        filter.selectItem(at: index)
        applyEnzymeSearch()
    }

    /// The cutters whose name contains the search text, and with "Only enzymes
    /// on the map", only those the map shows.
    private func applyEnzymeSearch() {
        let query = enzymeSearch.stringValue.trimmingCharacters(in: .whitespaces)
        let mapOnly = onMapOnly.state == .on
        enzymes = cutters.filter { e in
            (query.isEmpty || e.name.range(of: query, options: [.caseInsensitive, .diacriticInsensitive]) != nil)
                && (!mapOnly || (e.passesFilter && !e.hidden))
        }
        enzymeTable.reloadData()
    }

    @objc private func enzymeSearchChanged(_ sender: NSSearchField) {
        applyEnzymeSearch()
    }

    @objc private func onMapOnlyChanged(_ sender: NSButton) {
        UserDefaults.standard.set(sender.state == .on, forKey: Self.onMapOnlyKey)
        applyEnzymeSearch()
    }

    private func syncSelection() {
        syncing = true
        defer { syncing = false }
        if let row = features.firstIndex(where: { $0.index == document.activeFeature }) {
            featureTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            featureTable.scrollRowToVisible(row)
        } else {
            featureTable.deselectAll(nil)
        }
    }

    @objc private func filterChanged(_ sender: NSPopUpButton) {
        document.maxCuts = [1, 2, 3, 0][max(0, min(3, sender.indexOfSelectedItem))]
    }

    @objc private func enzymeToggled(_ sender: NSButton) {
        let row = sender.tag
        guard row >= 0 && row < enzymes.count else { return }
        document.setEnzyme(enzymes[row].name, hidden: sender.state == .off)
    }

    // MARK: NSTableViewDataSource / Delegate

    func numberOfRows(in tableView: NSTableView) -> Int {
        if tableView === primerTable { return primers.count }
        return tableView === featureTable ? features.count : enzymes.count
    }

    /// "start–end →" for a span on the sequence, 1-based, wrapping on a circle.
    private func location(start: Int, length: Int, strand: Int32) -> String {
        let n = document.length
        var end = start + length
        if end > n { end -= n }
        let arrow = strand == Int32(OC_STRAND_FORWARD) ? " →" : strand == Int32(OC_STRAND_REVERSE) ? " ←" : ""
        return "\(start + 1)–\(end)\(arrow)"
    }

    /// `content` in a cell, centred vertically in the row and filling its width.
    private func cell(_ content: NSView, centered: Bool = false) -> NSTableCellView {
        let cell = NSTableCellView()
        content.translatesAutoresizingMaskIntoConstraints = false
        cell.addSubview(content)
        var constraints = [content.centerYAnchor.constraint(equalTo: cell.centerYAnchor)]
        if centered {
            constraints.append(content.centerXAnchor.constraint(equalTo: cell.centerXAnchor))
        } else {
            // Labels fill the cell so their alignment and truncation apply.
            let trailing = content is NSTextField
                ? content.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -2)
                : content.trailingAnchor.constraint(lessThanOrEqualTo: cell.trailingAnchor, constant: -2)
            constraints += [content.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 2), trailing]
        }
        NSLayoutConstraint.activate(constraints)
        if let label = content as? NSTextField {
            label.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
            cell.textField = label
        }
        return cell
    }

    /// Melting temperature of the bases that bind (the 3′ part), or of the whole primer.
    private func tm(of p: PrimerInfo) -> Double {
        let binding = p.length > 0 && p.length < p.bases.count ? String(p.bases.suffix(p.length)) : p.bases
        var check = OCPrimerCheck()
        _ = takeString(oc_primer_check(binding, &check))
        return check.tm
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let id = tableColumn?.identifier.rawValue ?? ""
        return cell(content(tableView, id: id, row: row), centered: tableView === enzymeTable && id == "show")
    }

    private func content(_ tableView: NSTableView, id: String, row: Int) -> NSView {
        if tableView === primerTable {
            let p = primers[row]
            switch id {
            case "name":
                // A small arrow for the strand it reads: → forward, ← reverse.
                let reverse = p.strand == Int32(OC_STRAND_REVERSE)
                let arrow = NSImageView(image: NSImage(systemSymbolName: reverse ? "arrow.left" : "arrow.right",
                                                       accessibilityDescription: reverse ? "Reverse" : "Forward")
                                        ?? NSImage(size: NSSize(width: 12, height: 12)))
                arrow.contentTintColor = .systemOrange
                arrow.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 11, weight: .semibold)
                arrow.isHidden = p.length == 0
                let label = NSTextField(labelWithString: p.name)
                label.lineBreakMode = .byTruncatingTail
                label.toolTip = "\(p.name): 5′-\(p.bases)-3′"
                let stack = NSStackView(views: [arrow, label])
                stack.spacing = 5
                return stack
            case "tm":
                let t = tm(of: p)
                let label = NSTextField(labelWithString: t.isNaN ? "—" : String(format: "%.1f °C", t))
                label.textColor = .secondaryLabelColor
                label.toolTip = "Melting temperature of the bases that bind"
                return label
            default:
                let text = p.length > 0 ? location(start: p.start, length: p.length, strand: 0) : "doesn't bind"
                let label = NSTextField(labelWithString: text)
                label.textColor = .secondaryLabelColor
                return label
            }
        }
        if tableView === featureTable {
            let f = features[row]
            if id == "name" {
                let swatch = NSView(frame: NSRect(x: 0, y: 0, width: 12, height: 12))
                swatch.wantsLayer = true
                swatch.layer?.backgroundColor = f.color.cgColor
                swatch.layer?.cornerRadius = 3
                swatch.translatesAutoresizingMaskIntoConstraints = false
                swatch.widthAnchor.constraint(equalToConstant: 12).isActive = true
                swatch.heightAnchor.constraint(equalToConstant: 12).isActive = true
                let label = NSTextField(labelWithString: f.name)
                label.lineBreakMode = .byTruncatingTail
                label.toolTip = "\(f.name) (\(f.type))"
                let stack = NSStackView(views: [swatch, label])
                stack.spacing = 6
                return stack
            }
            let label = NSTextField(labelWithString: location(start: f.start, length: f.length, strand: f.strand))
            label.textColor = .secondaryLabelColor
            return label
        }
        let e = enzymes[row]
        switch id {
        case "show":
            let check = NSButton(checkboxWithTitle: "", target: self, action: #selector(enzymeToggled(_:)))
            check.tag = row
            check.state = (e.passesFilter && !e.hidden) ? .on : .off
            check.isEnabled = e.passesFilter
            return check
        case "name":
            let label = NSTextField(labelWithString: e.name)
            label.lineBreakMode = .byTruncatingTail
            label.toolTip = e.name
            label.textColor = e.passesFilter ? .labelColor : .tertiaryLabelColor
            return label
        case "site":
            let label = NSTextField(labelWithString: e.site)
            label.lineBreakMode = .byTruncatingTail
            label.toolTip = e.site   // long sites truncate in a narrow panel
            label.font = .monospacedSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
            label.textColor = .secondaryLabelColor
            return label
        default:
            let label = NSTextField(labelWithString: "\(e.cuts)")
            label.textColor = .secondaryLabelColor
            label.alignment = .right
            return label
        }
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !syncing, let table = notification.object as? NSTableView, table.selectedRow >= 0 else { return }
        if table === featureTable {
            document.activateFeature(features[table.selectedRow].index)
            return
        }
        if table === primerTable {
            // Select where the primer binds.
            let p = primers[table.selectedRow]
            if p.length > 0 { document.select(start: p.start, length: p.length) }
            return
        }
        // Enzyme: jump to its next visible site.
        let name = enzymes[table.selectedRow].name
        let sites = document.visibleSiteEnzymes
        let current = document.activeSite
        if let next = sites.indices.first(where: { sites[$0] == name && $0 > current })
            ?? sites.indices.first(where: { sites[$0] == name }) {
            document.activateSite(next)
        }
    }
}
