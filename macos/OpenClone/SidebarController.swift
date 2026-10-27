// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import OpenCloneCore

/// Features and Enzymes tabs.
final class SidebarController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    let view: NSTabView
    private let document: CoreDocument
    private let featureTable = NSTableView()
    private let enzymeTable = NSTableView()
    private let filter = NSPopUpButton(frame: .zero, pullsDown: false)
    private var features: [FeatureInfo] = []
    private var enzymes: [EnzymeInfo] = []
    private var syncing = false

    init(document: CoreDocument) {
        self.document = document
        view = NSTabView()
        super.init()
        buildFeaturesTab()
        buildEnzymesTab()
        document.observe { [weak self] what in
            guard let self = self else { return }
            if what & Change.sequence != 0 { self.reloadFeatures() }
            if what & (Change.sequence | Change.enzymes) != 0 { self.reloadEnzymes() }
            if what & Change.selection != 0 { self.syncSelection() }
        }
        reloadFeatures()
        reloadEnzymes()
    }

    private func makeTable(_ table: NSTableView, columns: [(String, String, CGFloat)]) -> NSScrollView {
        for (id, title, width) in columns {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(id))
            column.title = title
            column.width = width
            table.addTableColumn(column)
        }
        table.style = .sourceList
        table.usesAlternatingRowBackgroundColors = false
        table.allowsMultipleSelection = false
        table.dataSource = self
        table.delegate = self
        let scroll = NSScrollView()
        scroll.documentView = table
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        return scroll
    }

    private func buildFeaturesTab() {
        let scroll = makeTable(featureTable, columns: [("name", "Feature", 150), ("location", "Location", 110)])
        let item = NSTabViewItem(identifier: "features")
        item.label = "Features"
        item.view = scroll
        view.addTabViewItem(item)
    }

    private func buildEnzymesTab() {
        filter.addItems(withTitles: ["Unique cutters", "Up to 2 cuts", "Up to 3 cuts", "All cutters"])
        filter.target = self
        filter.action = #selector(filterChanged(_:))
        let scroll = makeTable(enzymeTable, columns: [("show", "", 24), ("name", "Enzyme", 80),
                                                       ("site", "Site", 100), ("cuts", "Cuts", 40)])
        let credit = NSTextField(labelWithString: "Enzyme data: REBASE, rebase.neb.com")
        credit.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        credit.textColor = .secondaryLabelColor
        let stack = NSStackView(views: [filter, scroll, credit])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 8, bottom: 8, right: 8)
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -16).isActive = true
        let item = NSTabViewItem(identifier: "enzymes")
        item.label = "Enzymes"
        item.view = stack
        view.addTabViewItem(item)
    }

    private func reloadFeatures() {
        features = document.features.filter { $0.drawn }
        featureTable.reloadData()
    }

    private func reloadEnzymes() {
        enzymes = document.enzymes.filter { $0.cuts > 0 }
            .sorted { $0.name.lowercased() < $1.name.lowercased() }
        let index = [1: 0, 2: 1, 3: 2, 0: 3][document.maxCuts] ?? 0
        filter.selectItem(at: index)
        enzymeTable.reloadData()
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
        tableView === featureTable ? features.count : enzymes.count
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let id = tableColumn?.identifier.rawValue ?? ""
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
            let n = document.length
            var end = f.start + f.length
            if end > n { end -= n }
            let arrow = f.strand == Int32(OC_STRAND_FORWARD) ? " →" : f.strand == Int32(OC_STRAND_REVERSE) ? " ←" : ""
            let label = NSTextField(labelWithString: "\(f.start + 1)–\(end)\(arrow)")
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
            label.textColor = e.passesFilter ? .labelColor : .tertiaryLabelColor
            return label
        case "site":
            let label = NSTextField(labelWithString: e.site)
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
