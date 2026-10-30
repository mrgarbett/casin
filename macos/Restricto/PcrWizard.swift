// SPDX-License-Identifier: GPL-3.0-or-later
// The PCR wizard: a window with the reaction's inputs on the left (the two
// primers and the polymerase) and the product on the right, drawn as it will
// be: its map and sequence, tails and all. The template's window selects the
// stretch that gets amplified. Open Product opens the product as a new,
// unsaved sequence.
import AppKit
import RestrictoCore

final class PcrWizard: NSWindowController, NSWindowDelegate {
    private let template: CoreDocument
    /// The product as drawn on the right; refilled as the inputs change.
    private let preview = CoreDocument()
    private var hasProduct = false

    private let forwardPopup = NSPopUpButton()
    private let reversePopup = NSPopUpButton()
    private let polymerasePopup = NSPopUpButton()
    private let info = NSTextField(wrappingLabelWithString: "")
    private let program = NSTextField(wrappingLabelWithString: "")
    /// The line above the program when it's copied: polymerase, primers, size.
    private var programHeader = ""
    private let warnings = NSTextField(wrappingLabelWithString: "")
    private let openButton = NSButton(title: "Open Product", target: nil, action: nil)
    private let addPrimerButton = NSButton(title: "Add Primer", target: nil, action: nil)
    private let emptyLabel = NSTextField(wrappingLabelWithString: "")
    private let productHeading = NSTextField(labelWithString: "PRODUCT")
    private let programRow = NSStackView()
    private var mapView: MapView!
    private var sequenceView: SequenceView!
    private let productSplit = NSSplitView()
    /// Opens the template's Primers pane to add a primer.
    private let onAddPrimer: () -> Void
    /// The template's window: the product opens as a tab there.
    private weak var parentWindow: NSWindow?

    /// Wizards open now, one per template.
    private static var showing: [PcrWizard] = []

    /// Brings up the wizard for `template`, opening one if there is none.
    static func show(for template: CoreDocument, near parent: NSWindow?, onAddPrimer: @escaping () -> Void) {
        if let open = visible.first(where: { $0.template === template }) {
            open.window?.makeKeyAndOrderFront(nil)
            return
        }
        let wizard = PcrWizard(template: template, onAddPrimer: onAddPrimer)
        wizard.parentWindow = parent
        showing.append(wizard)
        if let parent = parent, let window = wizard.window {
            // Over the template's window, a little down and right, named as it is.
            window.setFrameTopLeftPoint(NSPoint(x: parent.frame.minX + 40, y: parent.frame.maxY - 40))
            window.title = "PCR · \(Self.withoutExtension(parent.title))"
        }
        wizard.showWindow(nil)
        // The map takes a little over half the height, the sequence the rest.
        wizard.window?.layoutIfNeeded()
        let split = wizard.productSplit
        split.setPosition(split.bounds.height * 0.55, ofDividerAt: 0)
    }

    private init(template: CoreDocument, onAddPrimer: @escaping () -> Void) {
        self.template = template
        self.onAddPrimer = onAddPrimer
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1040, height: 660),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable], backing: .buffered,
                              defer: false)
        window.title = "PCR · \(template.name)"
        window.minSize = NSSize(width: 760, height: 480)
        window.isReleasedWhenClosed = false
        // A tool window, never a tab: document windows tab together, and one
        // opening while this is key must not try to join it.
        window.tabbingMode = .disallowed
        super.init(window: window)
        window.delegate = self
        window.contentView = buildView()
        template.observe { [weak self] what in
            // New, changed or deleted primers on the template.
            guard let self = self, what & Change.sequence != 0 else { return }
            self.fillPrimers()
            self.refresh()
        }
        fillPrimers()
        refresh()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    // MARK: Layout

    private func buildView() -> NSView {
        func heading(_ text: String) -> NSTextField {
            let label = NSTextField(labelWithString: text)
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
            label.textColor = .secondaryLabelColor
            return label
        }
        let title = NSTextField(labelWithString: "PCR")
        title.font = .systemFont(ofSize: NSFont.systemFontSize + 2, weight: .semibold)
        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel(_:)))
        cancel.keyEquivalent = "\u{1b}"
        openButton.target = self
        openButton.action = #selector(openProduct(_:))
        openButton.bezelColor = .controlAccentColor
        openButton.keyEquivalent = "\r"
        let header = NSStackView(views: [title, NSView(), cancel, openButton])
        header.spacing = 8

        for popup in [forwardPopup, reversePopup, polymerasePopup] {
            popup.target = self
            popup.action = #selector(inputsChanged(_:))
        }
        for p in Polymerase.all { polymerasePopup.addItem(withTitle: p.name) }
        polymerasePopup.selectItem(at: Polymerase.all.firstIndex { $0.name == Polymerase.defaultName } ?? 0)

        info.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        program.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        program.textColor = .secondaryLabelColor
        warnings.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        warnings.textColor = .systemOrange
        addPrimerButton.target = self
        addPrimerButton.action = #selector(addPrimer(_:))
        addPrimerButton.controlSize = .small
        addPrimerButton.toolTip = "Add a primer in the template's Primers pane"

        for label in [productHeading] {
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
            label.textColor = .secondaryLabelColor
        }
        // The program's heading, with a button to copy it for the cycler's notes.
        let copy = NSButton(title: "Copy", target: self, action: #selector(copyProgram(_:)))
        copy.controlSize = .small
        copy.toolTip = "Copy the program as text"
        programRow.setViews([heading("SUGGESTED PROGRAM"), NSView(), copy], in: .leading)
        let inputs = NSStackView(views: [
            header,
            heading("FORWARD PRIMER"), forwardPopup,
            heading("REVERSE PRIMER"), reversePopup,
            addPrimerButton,
            heading("POLYMERASE"), polymerasePopup,
            productHeading, info, warnings,
            programRow, program,
        ])
        inputs.orientation = .vertical
        inputs.alignment = .leading
        inputs.spacing = 6
        inputs.edgeInsets = NSEdgeInsets(top: 14, left: 16, bottom: 16, right: 16)
        for v in [header, forwardPopup, reversePopup, polymerasePopup, info, warnings, programRow, program] as [NSView] {
            v.widthAnchor.constraint(equalTo: inputs.widthAnchor, constant: -32).isActive = true
        }
        inputs.setCustomSpacing(14, after: header)
        let sidebar = NSView()
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        inputs.translatesAutoresizingMaskIntoConstraints = false
        sidebar.addSubview(inputs)
        NSLayoutConstraint.activate([
            sidebar.widthAnchor.constraint(equalToConstant: 300),
            inputs.topAnchor.constraint(equalTo: sidebar.topAnchor),
            inputs.leadingAnchor.constraint(equalTo: sidebar.leadingAnchor),
            inputs.trailingAnchor.constraint(equalTo: sidebar.trailingAnchor),
            inputs.bottomAnchor.constraint(lessThanOrEqualTo: sidebar.bottomAnchor),
        ])

        // The product: map over sequence, neither editable.
        mapView = MapView(document: preview)
        sequenceView = SequenceView(document: preview)
        sequenceView.isEditable = false
        let scroll = NSScrollView()
        scroll.documentView = sequenceView
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = false
        scroll.contentView.postsFrameChangedNotifications = true
        NotificationCenter.default.addObserver(forName: NSView.frameDidChangeNotification, object: scroll.contentView,
                                               queue: .main) { [weak self] _ in self?.sequenceView.updateSize() }
        productSplit.isVertical = false
        productSplit.dividerStyle = .thin
        productSplit.addArrangedSubview(mapView)
        productSplit.addArrangedSubview(scroll)
        // Extra height goes to both; the sequence gives way last.
        productSplit.setHoldingPriority(.defaultLow, forSubviewAt: 0)
        productSplit.setHoldingPriority(.defaultLow - 1, forSubviewAt: 1)
        mapView.heightAnchor.constraint(greaterThanOrEqualToConstant: 200).isActive = true
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 140).isActive = true

        // Shown instead of the product when there is none.
        emptyLabel.alignment = .center
        emptyLabel.textColor = .secondaryLabelColor
        emptyLabel.font = .systemFont(ofSize: NSFont.systemFontSize + 1)

        let content = NSView()
        for v in [productSplit, emptyLabel] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            content.addSubview(v)
        }
        NSLayoutConstraint.activate([
            productSplit.topAnchor.constraint(equalTo: content.topAnchor),
            productSplit.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            productSplit.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            productSplit.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            emptyLabel.centerYAnchor.constraint(equalTo: content.centerYAnchor),
            emptyLabel.centerXAnchor.constraint(equalTo: content.centerXAnchor),
            emptyLabel.widthAnchor.constraint(lessThanOrEqualTo: content.widthAnchor, constant: -80),
        ])

        let edge = NSBox()
        edge.boxType = .separator
        edge.widthAnchor.constraint(equalToConstant: 1).isActive = true
        let root = NSStackView(views: [sidebar, edge, content])
        root.orientation = .horizontal
        root.spacing = 0
        root.alignment = .top
        for v in [sidebar, edge, content] as [NSView] {
            v.heightAnchor.constraint(equalTo: root.heightAnchor).isActive = true
        }
        return root
    }

    // MARK: Inputs

    private struct Choice {
        let index: Int
        let start: Int
        let length: Int
        let forward: Bool
    }

    /// Lists the template's primers in both popups, keeping what was picked
    /// when it's still there; at first, a forward and a reverse primer.
    private func fillPrimers() {
        let keepForward = (forwardPopup.selectedItem?.representedObject as? NSNumber)?.intValue
        let keepReverse = (reversePopup.selectedItem?.representedObject as? NSNumber)?.intValue
        let primers = template.primers
        for (popup, wantForward) in [(forwardPopup, true), (reversePopup, false)] {
            popup.removeAllItems()
            popup.autoenablesItems = false
            for p in primers {
                let binds = p.length > 0
                let forward = p.strand == Int32(OC_STRAND_FORWARD)
                let place = binds ? "\((p.start + 1).formatted())–\((p.start + p.length).formatted())" : "doesn't bind"
                popup.addItem(withTitle: "\(p.name)   \(binds ? (forward ? "→" : "←") : "") \(place)")
                popup.lastItem?.representedObject = NSNumber(value: p.index)
                // Only primers reading the right way can be picked.
                popup.lastItem?.isEnabled = binds && forward == wantForward
            }
            if popup.numberOfItems == 0 {
                popup.addItem(withTitle: wantForward ? "No forward primer" : "No reverse primer")
                popup.lastItem?.isEnabled = false
            }
            let keep = wantForward ? keepForward : keepReverse
            if let keep = keep, let i = popup.itemArray.firstIndex(where: {
                ($0.representedObject as? NSNumber)?.intValue == keep && $0.isEnabled
            }) {
                popup.selectItem(at: i)
            } else if let i = popup.itemArray.firstIndex(where: { $0.isEnabled }) {
                popup.selectItem(at: i)
            }
        }
    }

    private func choice(_ popup: NSPopUpButton) -> Choice? {
        guard let item = popup.selectedItem, item.isEnabled,
              let index = (item.representedObject as? NSNumber)?.intValue,
              let p = template.primers.first(where: { $0.index == index }), p.length > 0 else { return nil }
        return Choice(index: index, start: p.start, length: p.length, forward: p.strand == Int32(OC_STRAND_FORWARD))
    }

    @objc private func inputsChanged(_ sender: Any?) { refresh() }

    // MARK: Preview

    /// Makes the product from the picked primers, draws it, and fills in the
    /// readout and the suggested program.
    private func refresh() {
        let primers = template.primers
        let forward = choice(forwardPopup), reverse = choice(reversePopup)
        var lines: [String] = []
        var warn: [String] = []
        hasProduct = false
        if let f = forward, let r = reverse {
            let out = CoreDocument()
            var err: UnsafeMutablePointer<CChar>?
            if oc_pcr_product(template.handle, Int32(f.index), Int32(r.index), out.handle, &err) {
                oc_document_copy(out.handle, preview.handle)
                oc_document_clear_selection(preview.handle)
                hasProduct = true
            } else {
                let text = takeString(err)
                warn.append(text.isEmpty ? "These primers make no product." : text.prefix(1).uppercased() + text.dropFirst() + ".")
            }
        }
        let fp = forward.flatMap { f in primers.first { $0.index == f.index } }
        let rp = reverse.flatMap { r in primers.first { $0.index == r.index } }
        let polymerase = Polymerase.all[max(0, polymerasePopup.indexOfSelectedItem)]
        if hasProduct, let f = forward, let r = reverse, let fp = fp, let rp = rp {
            let length = preview.length
            var end = r.start + r.length
            if end <= f.start { end += template.length }   // across the origin
            let copied = end - f.start
            lines.append("\(length.formatted()) bp")
            var copiedLine = "Copies \((f.start + 1).formatted())–\(((end - 1) % template.length + 1).formatted())"
                + " of \(template.name)"
            let tails = length - copied
            if tails > 0 { copiedLine += ", plus \(tails) bp of primer tails" }
            lines.append(copiedLine)
            // Annealing Tms: the parts that bind; tails join in from cycle 2.
            let tmF = annealingTm(fp), tmR = annealingTm(rp)
            if let a = tmF, let b = tmR {
                lines.append("Tm \(String(format: "%.1f", a)) °C (\(fp.name)) · \(String(format: "%.1f", b)) °C (\(rp.name))")
                if abs(a - b) > 5 {
                    let hotter = a > b ? fp.name : rp.name
                    warn.append("The annealing Tms differ by \(String(format: "%.1f", abs(a - b))) °C; "
                                + "fit \(hotter) closer in the Primers pane.")
                }
                program.stringValue = polymerase.program(tmLow: min(a, b), length: length)
                programHeader = "\(polymerase.name) · \(fp.name) + \(rp.name) · \(length.formatted()) bp"
            } else {
                program.stringValue = ""
            }
            let pair = takeString(oc_primer_pair_check(fp.bases, rp.bases))
            warn.append(contentsOf: pair.split(separator: "\n").filter { !$0.hasPrefix("Tm differs") }.map { String($0) })
            if length > polymerase.maxLength {
                warn.append("\(polymerase.name) is sold for products up to \((polymerase.maxLength / 1000).formatted()) kb.")
            }
            // The template's window shows what gets copied.
            template.select(start: f.start, length: copied)
        } else {
            program.stringValue = ""
        }
        let canPick = forwardPopup.itemArray.contains { $0.isEnabled } && reversePopup.itemArray.contains { $0.isEnabled }
        if !canPick {
            emptyLabel.stringValue = "PCR needs a primer on each strand facing the other. Add them in the template's "
                + "Primers pane, then pick them here."
        } else if !hasProduct {
            emptyLabel.stringValue = "No product with these primers."
        }
        emptyLabel.isHidden = hasProduct
        productSplit.isHidden = !hasProduct
        info.stringValue = lines.joined(separator: "\n")
        info.isHidden = lines.isEmpty
        warnings.stringValue = warn.joined(separator: "\n")
        warnings.isHidden = warn.isEmpty
        program.isHidden = program.stringValue.isEmpty
        programRow.isHidden = program.isHidden
        productHeading.isHidden = info.isHidden && warnings.isHidden
        openButton.isEnabled = hasProduct
    }

    private func annealingTm(_ p: PrimerInfo) -> Double? {
        var check = OCPrimerCheck()
        oc_string_free(oc_primer_check(String(p.bases.suffix(p.length)), &check))
        return check.tm.isNaN ? nil : check.tm
    }

    // MARK: Actions

    @objc private func openProduct(_ sender: Any?) {
        guard hasProduct else { NSSound.beep(); return }
        let product = CoreDocument()
        oc_document_copy(preview.handle, product.handle)
        oc_document_clear_selection(product.handle)
        // Open it from the template's window, so it tabs there, then close.
        parentWindow?.makeKeyAndOrderFront(nil)
        SequenceDocument.open(product)
        close()
    }

    @objc private func cancel(_ sender: Any?) { close() }

    @objc private func copyProgram(_ sender: Any?) {
        guard !program.stringValue.isEmpty else { NSSound.beep(); return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(programHeader + "\n" + program.stringValue, forType: .string)
    }

    @objc private func addPrimer(_ sender: Any?) { onAddPrimer() }

    /// File > New from Bases works from the template's window; pass it on
    /// rather than greying it out while the wizard is in front.
    @objc func newFromBases(_ sender: Any?) {
        (parentWindow?.windowController as? DocumentWindowController)?.newFromBases(sender)
    }

    /// "pOC-demo.dna" as "pOC-demo"; only sequence-file extensions go, so a
    /// name like "pUC19 v2.1" stays whole.
    private static func withoutExtension(_ title: String) -> String {
        let ext = (title as NSString).pathExtension.lowercased()
        let known: Set<String> = ["dna", "gb", "gbk", "genbank", "embl", "emb", "fasta", "fa", "fna", "ape", "seq"]
        return known.contains(ext) ? (title as NSString).deletingPathExtension : title
    }

    func windowWillClose(_ notification: Notification) {
        // Let go after the close finishes: dropping the last reference here
        // would free the window while it's still closing.
        DispatchQueue.main.async { Self.showing.removeAll { $0 === self } }
    }

    // MARK: Launch check

    /// Wizards not yet closed (a closed one lingers in `showing` until the next turn).
    private static var visible: [PcrWizard] { showing.filter { $0.window?.isVisible == true } }
    static var isAnyOpen: Bool { !visible.isEmpty }
    static var openWindows: [NSWindow] { visible.compactMap { $0.window } }
    /// The product length each open wizard shows (0 for none).
    static var productLengths: [Int] { visible.map { $0.hasProduct ? $0.preview.length : 0 } }
    static func openAll() { visible.forEach { $0.openProduct(nil) } }
    static func cancelAll() { visible.forEach { $0.cancel(nil) } }
}

/// A polymerase's cycling: the temperatures and times its maker suggests,
/// and the longest product it's sold for.
private struct Polymerase {
    let name: String
    let denature: Int            // °C
    let initialSeconds: Int
    let denatureSeconds: Int
    let annealOffset: Double     // from the lower annealing Tm
    let annealSeconds: Int
    let extend: Int              // °C
    let secondsPerKb: Int
    let minExtendSeconds: Int
    let maxLength: Int
    let finalMinutes: Int

    static let defaultName = "Q5 / Phusion (high fidelity)"
    static let all: [Polymerase] = [
        Polymerase(name: "Q5 / Phusion (high fidelity)", denature: 98, initialSeconds: 30, denatureSeconds: 10,
                   annealOffset: 3, annealSeconds: 20, extend: 72, secondsPerKb: 30, minExtendSeconds: 15,
                   maxLength: 20_000, finalMinutes: 2),
        Polymerase(name: "Taq", denature: 95, initialSeconds: 120, denatureSeconds: 30, annealOffset: -5,
                   annealSeconds: 30, extend: 68, secondsPerKb: 60, minExtendSeconds: 30, maxLength: 5_000,
                   finalMinutes: 5),
    ]

    /// The cycling for a product of `length` bp whose primers' lower annealing Tm is `tmLow`.
    func program(tmLow: Double, length: Int) -> String {
        let anneal = Int(min(72, max(45, (tmLow + annealOffset).rounded())))
        let perKb = (length * secondsPerKb + 999) / 1000
        let extendSeconds = max(minExtendSeconds, (perKb + 4) / 5 * 5)
        func time(_ s: Int) -> String { s >= 60 && s % 60 == 0 ? "\(s / 60) min" : (s > 60 ? "\(s / 60) min \(s % 60) s" : "\(s) s") }
        return """
        \(denature) °C  \(time(initialSeconds))
        30 cycles:
          \(denature) °C  \(time(denatureSeconds))
          \(anneal) °C  \(time(annealSeconds))
          \(extend) °C  \(time(extendSeconds))
        \(extend) °C  \(finalMinutes) min
        """
    }
}
