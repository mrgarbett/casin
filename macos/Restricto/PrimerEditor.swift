// SPDX-License-Identifier: GPL-3.0-or-later
// Adding a primer, in the right panel's Primers pane: the primer's name,
// description, colour and bases, with codons, enzyme sites and tags to insert
// into it. While it binds, the window's map and sequence draw it where it
// would go, as it will look; Add puts it on the document as one undo step.
import AppKit
import RestrictoCore

final class PrimerEditor: NSViewController, NSTextViewDelegate, NSTextFieldDelegate {
    /// Called once the primer was added or the editor cancelled.
    var onClose: (() -> Void)?

    private let document: CoreDocument
    private var site: OCPrimerSite?
    /// Set once the primer is added or cancelled, so later changes don't bring the preview back.
    private var finished = false

    private let nameField = NSTextField(string: "")
    private let descriptionField = NSTextField(string: "")
    private let colorWell = NSColorWell(style: .minimal)
    private var colorChosen = false
    private let sequenceText = NSTextView()
    /// The sequence box's own undo, apart from the document's.
    private let textUndo = UndoManager()
    /// On when the bases are the reverse complement of what was typed, so the
    /// primer binds the other strand; inserts then go in reverse complemented
    /// too, so they still read on the strand they were typed on.
    private let flipSwitch = NSSwitch()
    private let aminoAcidPopup = NSPopUpButton()
    private let codonPopup = NSPopUpButton()
    private let enzymePopup = NSPopUpButton()
    private let tagPopup = NSPopUpButton()
    private let info = NSTextField(wrappingLabelWithString: "")
    private let warnings = NSTextField(wrappingLabelWithString: "")
    private let addButton = NSButton(title: "Add", target: nil, action: nil)
    private let initialBases: String

    /// Editors showing now, for the launch check.
    private static var showing: [PrimerEditor] = []

    /// An editor for a new primer on `document`, filled in with `bases` (5'->3') if given.
    init(document: CoreDocument, bases: String = "") {
        self.document = document
        initialBases = bases
        super.init(nibName: nil, bundle: nil)
        Self.showing.append(self)
        document.observe { [weak self] what in
            // An edit to the sequence drops the preview; find the site again,
            // leaving the caret where the edit put it.
            guard let self = self, !self.finished, what & Change.sequence != 0 else { return }
            self.refresh(select: false)
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func loadView() {
        view = buildView()
        sequenceText.string = initialBases
        // A 5' tail is what gets added most, so start at the 5' end.
        sequenceText.setSelectedRange(NSRange(location: 0, length: 0))
        refresh()
    }

    /// Puts the cursor in the sequence box.
    func focus() {
        view.window?.makeFirstResponder(sequenceText)
    }

    /// Replaces the bases, as when a new selection is sent here.
    func setBases(_ bases: String) {
        sequenceText.string = bases
        flipSwitch.state = .off
        sequenceText.setSelectedRange(NSRange(location: 0, length: 0))
        refresh()
    }

    // MARK: Layout

    private func buildView() -> NSView {
        func heading(_ text: String) -> NSTextField {
            let label = NSTextField(labelWithString: text)
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
            label.textColor = .secondaryLabelColor
            return label
        }
        let title = NSTextField(labelWithString: "New Primer")
        title.font = .systemFont(ofSize: NSFont.systemFontSize + 2, weight: .semibold)

        nameField.placeholderString = "primer"
        nameField.stringValue = nextPrimerName()
        nameField.delegate = self
        descriptionField.placeholderString = "Optional"
        descriptionField.delegate = self

        colorWell.color = NSColor(srgbRed: 0xe6 / 255.0, green: 0x61 / 255.0, blue: 0, alpha: 1)
        colorWell.target = self
        colorWell.action = #selector(colorChanged(_:))
        colorWell.toolTip = "The primer's colour on the map and sequence"
        let colorRow = NSStackView(views: [colorWell, NSTextField(labelWithString: "Colour")])
        colorRow.spacing = 8

        sequenceText.isRichText = false
        sequenceText.font = .monospacedSystemFont(ofSize: 13, weight: .regular)
        sequenceText.isAutomaticQuoteSubstitutionEnabled = false
        sequenceText.isAutomaticSpellingCorrectionEnabled = false
        sequenceText.isContinuousSpellCheckingEnabled = false
        sequenceText.isAutomaticTextReplacementEnabled = false
        sequenceText.allowsUndo = true
        sequenceText.delegate = self
        sequenceText.textContainerInset = NSSize(width: 4, height: 6)
        let textScroll = NSScrollView()
        textScroll.documentView = sequenceText
        textScroll.hasVerticalScroller = true
        textScroll.borderType = .bezelBorder
        sequenceText.autoresizingMask = [.width]
        sequenceText.isVerticallyResizable = true
        sequenceText.textContainer?.widthTracksTextView = true
        textScroll.heightAnchor.constraint(equalToConstant: 96).isActive = true

        flipSwitch.controlSize = .small
        flipSwitch.target = self
        flipSwitch.action = #selector(flipChanged(_:))
        let flipLabel = NSTextField(labelWithString: "Reverse complement")
        flipLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        let flipRow = NSStackView(views: [flipSwitch, flipLabel])
        flipRow.spacing = 6
        flipRow.toolTip = "Bind the other strand. What you insert still reads on the strand you typed."

        info.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        info.textColor = .labelColor
        warnings.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        warnings.textColor = .systemOrange

        // Codon: an amino acid, then one of its codons.
        for aa in PrimerParts.aminoAcids { aminoAcidPopup.addItem(withTitle: aa.name) }
        aminoAcidPopup.target = self
        aminoAcidPopup.action = #selector(aminoAcidChanged(_:))
        fillCodons()
        let codonRow = insertRow([aminoAcidPopup, codonPopup], action: #selector(insertCodon(_:)))
        codonPopup.widthAnchor.constraint(equalToConstant: 100).isActive = true

        // Enzyme site: the unambiguous sites, with how often each cuts here.
        for e in document.enzymes.filter({ $0.site.allSatisfy { "ACGT".contains($0) } })
            .sorted(by: { $0.name.localizedStandardCompare($1.name) == .orderedAscending }) {
            let cuts = e.cuts == 0 ? "doesn't cut" : "cuts \(e.cuts)×"
            enzymePopup.addItem(withTitle: "\(e.name)  \(e.site)  · \(cuts)")
            enzymePopup.lastItem?.representedObject = e.site
        }
        if let i = enzymePopup.itemArray.firstIndex(where: { $0.title.hasPrefix("EcoRI ") }) {
            enzymePopup.selectItem(at: i)
        }
        let enzymeRow = insertRow([enzymePopup], action: #selector(insertEnzymeSite(_:)))

        // Tags and protease sites.
        for tag in PrimerParts.tags {
            tagPopup.addItem(withTitle: "\(tag.name)  · \(tag.protein)")
            tagPopup.lastItem?.representedObject = tag.bases
        }
        let tagRow = insertRow([tagPopup], action: #selector(insertTag(_:)))

        let insertHint = NSTextField(wrappingLabelWithString: "Inserts go in at the cursor, in lowercase.")
        insertHint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        insertHint.textColor = .tertiaryLabelColor

        // Return and Esc work from the fields, not as window-wide keys, so
        // they don't take over the rest of the window (Find, for one).
        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel(_:)))
        addButton.target = self
        addButton.action = #selector(accept(_:))
        addButton.bezelColor = .controlAccentColor
        addButton.toolTip = "Add the primer (Return)"
        let buttons = NSStackView(views: [NSView(), cancel, addButton])
        buttons.spacing = 8

        let stack = NSStackView(views: [
            title,
            heading("NAME"), nameField,
            heading("DESCRIPTION"), descriptionField,
            heading("TAG"), colorRow,
            heading("SEQUENCE (5′ → 3′)"), textScroll, flipRow,
            info, warnings,
            heading("CODON"), codonRow,
            heading("ENZYME SITE"), enzymeRow,
            heading("TAG OR PROTEASE SITE"), tagRow,
            insertHint,
        ])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        for v in [title, nameField, descriptionField, colorRow, flipRow, codonRow, enzymeRow] as [NSView] {
            stack.setCustomSpacing(14, after: v)
        }
        stack.setCustomSpacing(18, after: warnings)
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 10, bottom: 12, right: 14)
        for v in [nameField, descriptionField, textScroll, info, warnings, codonRow, enzymeRow, tagRow,
                  insertHint] as [NSView] {
            v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -24).isActive = true
        }

        // The inputs scroll when the panel is short; the buttons stay put.
        let content = FlippedView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(stack)
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = content
        content.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            stack.topAnchor.constraint(equalTo: content.topAnchor),
            stack.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            content.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
            content.trailingAnchor.constraint(equalTo: scroll.contentView.trailingAnchor),
            content.topAnchor.constraint(equalTo: scroll.contentView.topAnchor),
        ])

        let root = NSView()
        for v in [scroll, buttons] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: root.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: buttons.topAnchor, constant: -10),
            buttons.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 10),
            buttons.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -14),
            buttons.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -12),
        ])
        return root
    }

    /// Popups that fill the row, then an Insert button.
    private func insertRow(_ popups: [NSPopUpButton], action: Selector) -> NSStackView {
        for p in popups {
            p.controlSize = .small
            p.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            p.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        }
        let insert = NSButton(title: "Insert", target: self, action: action)
        insert.controlSize = .small
        insert.setContentHuggingPriority(.required, for: .horizontal)
        let row = NSStackView(views: popups + [insert])
        row.spacing = 6
        return row
    }

    // MARK: Preview

    /// The primer's bases as typed, without spaces or line breaks.
    private var bases: String {
        sequenceText.string.filter { !$0.isWhitespace }
    }

    private var hexColor: String? {
        guard colorChosen else { return nil }
        let c = colorWell.color.usingColorSpace(.sRGB) ?? colorWell.color
        func byte(_ v: CGFloat) -> Int { Int((min(1, max(0, v)) * 255).rounded()) }
        return String(format: "#%02x%02x%02x", byte(c.redComponent), byte(c.greenComponent), byte(c.blueComponent))
    }

    private var primerName: String {
        let name = nameField.stringValue.trimmingCharacters(in: .whitespaces)
        return name.isEmpty ? "primer" : name
    }

    /// Finds where the primer binds and draws it there on the document's views,
    /// selecting the site (which brings it into view) when `select`.
    private func refresh(select: Bool = true) {
        guard !finished else { return }
        let typed = bases
        var sites = [OCPrimerSite](repeating: OCPrimerSite(), count: 8)
        let count = typed.isEmpty ? 0 : Int(oc_primer_sites(document.handle, typed, &sites, Int32(sites.count)))
        site = count > 0 ? sites[0] : nil

        var lines: [String] = []
        var warn: [String] = []
        var check = OCPrimerCheck()
        let checkWarnings = typed.isEmpty ? "" : takeString(oc_primer_check(typed, &check))
        if !typed.isEmpty {
            var line = "\(check.length) nt · GC \(String(format: "%.0f", check.gc * 100))%"
            if let s = site {
                // Tm of the part that anneals; a 5' tail doesn't count.
                let annealing = String(typed.suffix(Int(s.length)))
                var a = OCPrimerCheck()
                oc_string_free(oc_primer_check(annealing, &a))
                if !a.tm.isNaN { line += " · Tm \(String(format: "%.1f", a.tm)) °C" }
            } else if !check.tm.isNaN {
                line += " · Tm \(String(format: "%.1f", check.tm)) °C"
            }
            lines.append(line)
        }
        if let s = site {
            let reverse = s.strand == Int32(OC_STRAND_REVERSE)
            lines.append("Binds \((Int(s.start) + 1).formatted())–\((Int(s.start + s.length)).formatted()) "
                         + (reverse ? "on the bottom strand ←" : "on the top strand →"))
            let tail = typed.count - Int(s.length)
            if tail > 0 { lines.append("\(tail)-nt 5′ tail") }
            if s.mismatches > 0 { warn.append("\(s.mismatches) mismatch\(s.mismatches == 1 ? "" : "es") where it anneals.") }
            if count > 1 { warn.append("Also binds \(count - 1) other place\(count == 2 ? "" : "s"); the best site is shown.") }
            oc_document_set_pending_primer(document.handle, primerName, typed, hexColor, &sites[0])
            if select, !document.hasSelection || document.selectionStart != Int(s.start)
                || document.selectionLength != Int(s.length) {
                document.select(start: Int(s.start), length: Int(s.length))
            }
        } else {
            oc_document_clear_pending_primer(document.handle)
            if !typed.isEmpty {
                warn.append(typed.count < 15 ? "Type at least 15 bases." : "It doesn't bind this sequence.")
            } else {
                lines.append("Type or paste a primer; the map and sequence show where it binds.")
            }
        }
        if !checkWarnings.isEmpty {
            warn.append(contentsOf: checkWarnings.split(separator: "\n").map { String($0) })
        }
        info.stringValue = lines.joined(separator: "\n")
        info.textColor = typed.isEmpty ? .secondaryLabelColor : .labelColor
        warnings.stringValue = warn.joined(separator: "\n")
        warnings.isHidden = warn.isEmpty
        info.isHidden = lines.isEmpty
        addButton.isEnabled = site != nil
    }

    private func nextPrimerName() -> String {
        let names = Set(document.primers.map { $0.name })
        var i = names.count + 1
        while names.contains("primer\(i)") { i += 1 }
        return "primer\(i)"
    }

    // MARK: Actions

    func textDidChange(_ notification: Notification) { refresh() }
    func controlTextDidChange(_ obj: Notification) { refresh() }

    func undoManager(for view: NSTextView) -> UndoManager? { textUndo }

    /// Return adds the primer and Esc cancels, from the sequence box…
    func textView(_ textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        handle(selector)
    }

    /// …and from the name and description fields.
    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        handle(selector)
    }

    private func handle(_ selector: Selector) -> Bool {
        switch selector {
        case #selector(NSResponder.insertNewline(_:)):
            accept(nil)
            return true
        case #selector(NSResponder.cancelOperation(_:)):
            cancel(nil)
            return true
        default:
            return false
        }
    }

    /// Edit > Undo while the editor has the focus: the sequence box's typing first.
    @objc func undoEdit(_ sender: Any?) {
        if textUndo.canUndo { textUndo.undo() } else { oc_document_undo(document.handle) }
    }

    @objc func redoEdit(_ sender: Any?) {
        if textUndo.canRedo { textUndo.redo() } else { oc_document_redo(document.handle) }
    }

    @objc private func colorChanged(_ sender: Any?) {
        colorChosen = true
        refresh()
    }

    @objc private func flipChanged(_ sender: Any?) {
        // Keep the cursor on the same base of the flipped primer.
        let text = sequenceText.string
        let caret = sequenceText.selectedRange()
        sequenceText.string = PrimerParts.reverseComplement(text)
        let length = (text as NSString).length
        sequenceText.setSelectedRange(NSRange(location: max(0, length - caret.location - caret.length),
                                              length: caret.length))
        refresh()
    }

    @objc private func aminoAcidChanged(_ sender: Any?) { fillCodons() }

    private func fillCodons() {
        codonPopup.removeAllItems()
        let aa = PrimerParts.aminoAcids[max(0, aminoAcidPopup.indexOfSelectedItem)]
        for (i, codon) in aa.codons.enumerated() {
            codonPopup.addItem(withTitle: i == 0 && aa.codons.count > 1 ? "\(codon) (E. coli)" : codon)
            codonPopup.lastItem?.representedObject = codon
        }
    }

    @objc private func insertCodon(_ sender: Any?) { insert(codonPopup.selectedItem?.representedObject as? String) }
    @objc private func insertEnzymeSite(_ sender: Any?) { insert(enzymePopup.selectedItem?.representedObject as? String) }
    @objc private func insertTag(_ sender: Any?) { insert(tagPopup.selectedItem?.representedObject as? String) }

    /// Puts `bases` (as read on the typed strand) in at the cursor, as one undo step.
    private func insert(_ bases: String?) {
        guard let bases = bases, !bases.isEmpty else { NSSound.beep(); return }
        let text = flipSwitch.state == .on ? PrimerParts.reverseComplement(bases) : bases
        view.window?.makeFirstResponder(sequenceText)
        sequenceText.insertText(text.lowercased(), replacementRange: sequenceText.selectedRange())
    }

    @objc private func accept(_ sender: Any?) {
        guard var s = site else { NSSound.beep(); return }
        finished = true
        oc_document_clear_pending_primer(document.handle)
        let description = descriptionField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        oc_document_add_primer_at(document.handle, primerName, bases, hexColor, description, &s)
        document.select(start: Int(s.start), length: Int(s.length))
        close()
    }

    @objc private func cancel(_ sender: Any?) {
        finished = true
        oc_document_clear_pending_primer(document.handle)
        close()
    }

    private func close() {
        Self.showing.removeAll { $0 === self }
        onClose?()
    }

    // MARK: Launch check

    static var isAnyOpen: Bool { !showing.isEmpty }
    /// Presses Add on each open editor.
    static func acceptAll() { showing.forEach { $0.accept(nil) } }
    /// Inserts the first tag (His6) at each open editor's cursor.
    static func insertFirstTag() {
        showing.forEach {
            $0.tagPopup.selectItem(at: 0)
            $0.insertTag(nil)
        }
    }
}

/// Top-down coordinates, so the inputs start at the top of their scroll view.
private final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}
