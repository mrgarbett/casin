// SPDX-License-Identifier: GPL-3.0-or-later
// Analyze Translation: a popover beside the selected bases with the protein
// they encode (position, mass, pI, extinction, instability, charge, GRAVY),
// following the selection while it is open. Show More widens it with the
// amino acid count.
import AppKit
import RestrictoCore

final class TranslationPopover: NSViewController, NSPopoverDelegate {
    private static var current: NSPopover?

    /// Shows the popover for `document`'s selection, its arrow at `rect` of `view`.
    static func show(for document: CoreDocument, relativeTo rect: NSRect, of view: NSView) {
        current?.close()
        let controller = TranslationPopover(document: document)
        let popover = NSPopover()
        popover.behavior = .transient
        popover.animates = true
        popover.contentViewController = controller
        popover.delegate = controller
        controller.popover = popover
        current = popover
        popover.show(relativeTo: rect, of: view, preferredEdge: .maxY)
    }

    // For the launch check (UISmokeTest).
    static var openWindow: NSWindow? { current?.contentViewController?.view.window }
    static func toggleMore() { (current?.contentViewController as? TranslationPopover)?.toggleMore(nil) }
    static func closeAll() { current?.close() }
    /// Kept open when the app goes inactive, as a launch check's app can.
    static func holdOpen() { current?.behavior = .applicationDefined }

    private let document: CoreDocument
    private weak var popover: NSPopover?
    private var protein = ""
    private var showingMore = false

    private let header = NSTextField(labelWithString: "Translation")
    private let grid = NSGridView()
    private let note = NSTextField(wrappingLabelWithString: "")
    private let counts = NSGridView()
    private let countsColumn = NSStackView()
    private let copyButton = NSButton(title: "Copy Protein", target: nil, action: nil)
    private let moreButton = NSButton(title: "Show More…", target: nil, action: nil)

    private init(document: CoreDocument) {
        self.document = document
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func loadView() {
        header.font = .boldSystemFont(ofSize: NSFont.systemFontSize + 1)
        header.lineBreakMode = .byTruncatingTail
        note.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        note.textColor = .systemOrange
        note.preferredMaxLayoutWidth = 320
        grid.rowSpacing = 6
        grid.columnSpacing = 18
        counts.rowSpacing = 2
        counts.columnSpacing = 10
        copyButton.target = self
        copyButton.action = #selector(copyProtein(_:))
        copyButton.controlSize = .small
        moreButton.target = self
        moreButton.action = #selector(toggleMore(_:))
        moreButton.controlSize = .small
        let buttons = NSStackView(views: [copyButton, NSView(), moreButton])
        buttons.distribution = .fill
        let main = NSStackView(views: [header, grid, note, buttons])
        main.orientation = .vertical
        main.alignment = .leading
        main.spacing = 10
        // Its own height, not the amino acid column's.
        for v in [main, grid, counts] as [NSView] { v.setContentHuggingPriority(.required, for: .vertical) }
        buttons.widthAnchor.constraint(equalTo: main.widthAnchor).isActive = true

        let countsHeader = NSTextField(labelWithString: "Amino acids")
        countsHeader.font = .boldSystemFont(ofSize: NSFont.systemFontSize + 1)
        countsColumn.setViews([countsHeader, counts], in: .leading)
        countsColumn.orientation = .vertical
        countsColumn.alignment = .leading
        countsColumn.spacing = 10
        countsColumn.isHidden = true

        let divider = NSBox()
        divider.boxType = .separator
        let row = NSStackView(views: [main, divider, countsColumn])
        row.alignment = .top
        row.spacing = 16
        row.edgeInsets = NSEdgeInsets(top: 14, left: 16, bottom: 14, right: 16)
        divider.isHidden = true
        divider.heightAnchor.constraint(equalTo: row.heightAnchor, constant: -28).isActive = true
        view = row
        document.observe { [weak self] what in
            if what & (Change.selection | Change.sequence) != 0 { self?.refresh() }
        }
        refresh()
    }

    func popoverDidClose(_ notification: Notification) {
        if Self.current === popover { Self.current = nil }
    }

    // MARK: Content

    private func label(_ text: String, indent: Bool = false) -> NSTextField {
        let l = NSTextField(labelWithString: indent ? "    " + text : text)
        l.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        l.textColor = .secondaryLabelColor
        return l
    }

    private func value(_ text: String) -> NSTextField {
        let v = NSTextField(labelWithString: text)
        v.font = .monospacedSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        v.isSelectable = true
        return v
    }

    private func setRows(_ grid: NSGridView, _ rows: [[NSView]]) {
        while grid.numberOfRows > 0 { grid.removeRow(at: 0) }
        for r in rows { grid.addRow(with: r) }
    }

    private func refresh() {
        var info = OCProteinInfo()
        let n = document.length
        guard let p = oc_document_selection_protein(document.handle, &info), info.codons > 0 else {
            protein = ""
            header.stringValue = "Translation"
            setRows(grid, [[label("Select 3 or more bases to translate.")]])
            note.isHidden = true
            copyButton.isEnabled = false
            setRows(counts, [])
            resize()
            return
        }
        protein = takeString(p)
        let start = document.selectionStart, length = document.selectionLength
        let end = (start + length - 1) % max(1, n) + 1
        let reverse = info.strand == Int32(OC_STRAND_REVERSE)
        let feature = document.features.first {
            $0.index == document.activeFeature && $0.start == start && $0.length == length
        }
        header.stringValue = feature?.name ?? "Translation"

        // The translation as read, its stop included: the C API leaves a final stop off.
        let read = protein.count < Int(info.codons) ? protein + "*" : protein
        let summary = read.count <= 12 ? read : "\(read.prefix(4))…\(read.suffix(4))"
        let mass = info.mass
        let abs = { (ext: Int32) in mass > 0 ? String(format: "%.3f", Double(ext) / mass) : "–" }
        let stable = info.instability > 40 ? "unstable" : "stable"
        setRows(grid, [
            [label("Position"), value("\((start + 1).formatted())–\(end.formatted()) (\(reverse ? "reverse" : "forward"))")],
            [label("Summary"), value("\(summary) \(info.codons) aa")],
            [label("Molecular weight"), value(String(format: "%.2f Da", mass))],
            [label("Isoelectric point (pI)"), value(String(format: "%.2f", info.pI))],
            [label("Charge at pH 7"), value(String(format: "%+.1f", info.charge_ph7))],
            [label("Extinction coefficient"), value("")],
            [label("Cys fully reduced", indent: true), value("\(Int(info.extinction_reduced)) M⁻¹cm⁻¹")],
            [label("Abs 0.1% (1 g/l)", indent: true), value(abs(info.extinction_reduced))],
            [label("Cys fully oxidized", indent: true), value("\(Int(info.extinction_cystines)) M⁻¹cm⁻¹")],
            [label("Abs 0.1% (1 g/l)", indent: true), value(abs(info.extinction_cystines))],
            [label("Instability index"), value(String(format: "%.2f (%@)", info.instability, stable))],
            [label("Aliphatic index"), value(String(format: "%.2f", info.aliphatic))],
            [label("GRAVY"), value(String(format: "%.3f", info.gravy))],
        ])
        grid.column(at: 0).xPlacement = .leading
        var notes: [String] = []
        if info.truncated { notes.append("A stop codon ends the protein early; the figures are up to it.") }
        if length % 3 != 0 { notes.append("The selection isn't a whole number of codons; the last \(length % 3) base(s) are left out.") }
        if info.unknown > 0 { notes.append("\(info.unknown) unknown residue(s) are left out of the mass.") }
        note.stringValue = notes.joined(separator: "\n")
        note.isHidden = notes.isEmpty
        copyButton.isEnabled = !protein.isEmpty
        fillCounts(info)
        resize()
    }

    private static let residues: [(Character, String)] = [
        ("A", "Ala"), ("R", "Arg"), ("N", "Asn"), ("D", "Asp"), ("C", "Cys"), ("Q", "Gln"), ("E", "Glu"),
        ("G", "Gly"), ("H", "His"), ("I", "Ile"), ("L", "Leu"), ("K", "Lys"), ("M", "Met"), ("F", "Phe"),
        ("P", "Pro"), ("S", "Ser"), ("T", "Thr"), ("W", "Trp"), ("Y", "Tyr"), ("V", "Val"),
    ]

    private func fillCounts(_ info: OCProteinInfo) {
        var c = info.counts
        let byLetter = withUnsafeBytes(of: &c) { Array($0.bindMemory(to: Int32.self)) }
        let count = { (aa: Character) in Int(byLetter[Int(aa.asciiValue! - 65)]) }
        let total = max(1, Self.residues.reduce(0) { $0 + count($1.0) })
        let pct = { (k: Int) in String(format: "%.1f%%", 100.0 * Double(k) / Double(total)) }
        var rows: [[NSView]] = Self.residues.map { aa, name in
            let k = count(aa)
            let bar = NSLevelIndicator()
            bar.levelIndicatorStyle = .continuousCapacity
            bar.maxValue = 1
            bar.doubleValue = Double(k) / Double(total) * 4   // a quarter of the residues fills it
            bar.controlSize = .small
            bar.widthAnchor.constraint(equalToConstant: 56).isActive = true
            return [label("\(name)  \(aa)"), value("\(k)"), value(pct(k)), bar]
        }
        let groups: [(String, String)] = [
            ("Acidic (D, E)", "DE"), ("Basic (K, R, H)", "KRH"),
            ("Polar (S, T, N, Q, C, Y)", "STNQCY"), ("Hydrophobic (A, V, I, L, M, F, W)", "AVILMFW"),
        ]
        rows.append([value(""), value(""), value(""), NSView()])
        for (title, letters) in groups {
            let k = letters.reduce(0) { $0 + count($1) }
            rows.append([label(title), value("\(k)"), value(pct(k)), NSView()])
        }
        setRows(counts, rows)
        counts.column(at: 1).xPlacement = .trailing
        counts.column(at: 2).xPlacement = .trailing
    }

    private func resize() {
        view.layoutSubtreeIfNeeded()
        let size = view.fittingSize
        if popover?.contentSize != size { popover?.contentSize = size }
    }

    // MARK: Actions

    @objc private func copyProtein(_ sender: Any?) {
        guard !protein.isEmpty else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(protein, forType: .string)
    }

    @objc private func toggleMore(_ sender: Any?) {
        showingMore.toggle()
        moreButton.title = showingMore ? "Show Less" : "Show More…"
        countsColumn.isHidden = !showingMore
        (view as? NSStackView)?.arrangedSubviews[1].isHidden = !showingMore
        resize()
    }
}
