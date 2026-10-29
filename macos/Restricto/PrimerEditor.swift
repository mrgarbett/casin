// SPDX-License-Identifier: GPL-3.0-or-later
// Adding a primer, in the right panel's Primers pane: the primer's name,
// description, colour and bases, with codons, enzyme sites and tags to insert
// into it (each marked under the bases with a line showing which way it reads).
// While it binds, the window's map and sequence draw it where it would go, as
// it will look; Add puts it on the document as one undo step.
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
    private let sequenceText = PrimerTextView(usingTextLayoutManager: false)
    /// On when the bases are the reverse complement of what was typed, so the
    /// primer binds the other strand; inserts then go in reverse complemented
    /// too, so they still read on the strand they were typed on.
    private let flipSwitch = NSSwitch()
    private let aminoAcidPopup = NSPopUpButton()
    private let codonPopup = NSPopUpButton()
    private let enzymePopup = NSPopUpButton()
    private let tagPopup = NSPopUpButton()
    /// Target Tm for the annealing part, and the stepper beside it.
    private let tmField = NSTextField(string: "60")
    private let tmStepper = NSStepper()
    /// Another primer to pair this one with: their product, Tm gap and dimers.
    private let partnerPopup = NSPopUpButton()
    /// Pads an enzyme site put at the 5' end with a few bases, so it cuts.
    private let padSwitch = NSButton(checkboxWithTitle: "Pad a 5′-end site so it cuts", target: nil, action: nil)
    private let info = NSTextField(wrappingLabelWithString: "")
    private let warnings = NSTextField(wrappingLabelWithString: "")
    private let addButton = NSButton(title: "Add", target: nil, action: nil)
    private let initialBases: String
    /// The selection before the editor moved it, put back on Cancel, and
    /// what the editor last selected.
    private let selectionBefore: (start: Int, length: Int)
    private var selectionShown: (start: Int, length: Int)?

    /// Editors showing now, for the launch check.
    private static var showing: [PrimerEditor] = []

    /// An editor for a new primer on `document`, filled in with `bases` (5'->3') if given.
    init(document: CoreDocument, bases: String = "") {
        self.document = document
        initialBases = bases
        selectionBefore = (document.selectionStart, document.hasSelection ? document.selectionLength : 0)
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
        sequenceText.setPlain(initialBases)
        // A 5' tail is what gets added most, so start at the 5' end.
        sequenceText.setSelectedRange(NSRange(location: 0, length: 0))
        refresh()
    }

    /// Puts the cursor in the sequence box.
    func focus() {
        view.window?.undoManager?.removeAllActions()
        view.window?.makeFirstResponder(sequenceText)
    }

    /// Replaces the bases, as when a new selection is sent here.
    func setBases(_ bases: String) {
        sequenceText.setPlain(bases)
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
        title.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

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
        sequenceText.setUpLook()
        let textScroll = NSScrollView()
        textScroll.documentView = sequenceText
        textScroll.hasVerticalScroller = true
        textScroll.borderType = .bezelBorder
        sequenceText.autoresizingMask = [.width]
        sequenceText.isVerticallyResizable = true
        sequenceText.textContainer?.widthTracksTextView = true
        textScroll.heightAnchor.constraint(equalToConstant: 110).isActive = true

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
            enzymePopup.lastItem?.representedObject = PrimerPart(kind: .enzymeSite, name: e.name, bases: e.site)
        }
        if let i = enzymePopup.itemArray.firstIndex(where: { $0.title.hasPrefix("EcoRI ") }) {
            enzymePopup.selectItem(at: i)
        }
        let enzymeRow = insertRow([enzymePopup], action: #selector(insertEnzymeSite(_:)))
        padSwitch.controlSize = .small
        padSwitch.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        padSwitch.state = .on
        padSwitch.toolTip = "Most enzymes cut poorly right at a DNA end; \(PrimerParts.sitePadding) goes in before a site at the 5′ end."

        // Target Tm: the annealing part grows or shrinks along the template to meet it.
        tmField.controlSize = .small
        tmField.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        tmField.alignment = .right
        tmField.target = self
        tmField.action = #selector(tmFieldChanged(_:))
        tmField.widthAnchor.constraint(equalToConstant: 40).isActive = true
        tmStepper.controlSize = .small
        // Low enough to reach the 15-base floor on most primers.
        tmStepper.minValue = 30
        tmStepper.maxValue = 80
        tmStepper.increment = 1
        tmStepper.integerValue = 60
        tmStepper.valueWraps = false
        tmStepper.target = self
        tmStepper.action = #selector(tmStepped(_:))
        let fit = NSButton(title: "Fit", target: self, action: #selector(fitToTm(_:)))
        fit.controlSize = .small
        fit.toolTip = "Grow or shrink the annealing part along the template to this Tm"
        let degrees = NSTextField(labelWithString: "°C")
        degrees.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        let tmRow = NSStackView(views: [tmField, degrees, tmStepper, NSView(), fit])
        tmRow.spacing = 4

        // Partner: none, or one of the document's primers.
        partnerPopup.controlSize = .small
        partnerPopup.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        partnerPopup.addItem(withTitle: "None")
        for p in document.primers where p.length > 0 {
            partnerPopup.addItem(withTitle: p.name)
            partnerPopup.lastItem?.representedObject = p.index
        }
        partnerPopup.target = self
        partnerPopup.action = #selector(partnerChanged(_:))

        // Tags and protease sites.
        for tag in PrimerParts.tags {
            tagPopup.addItem(withTitle: "\(tag.name)  · \(tag.protein)")
            tagPopup.lastItem?.representedObject = PrimerPart(kind: tag.isProteaseSite ? .proteaseSite : .tag,
                                                              name: tag.shortName, bases: tag.bases)
        }
        let tagRow = insertRow([tagPopup], action: #selector(insertTag(_:)))

        let insertHint = NSTextField(wrappingLabelWithString:
            "Inserts go in at the cursor, marked underneath with which way they read.")
        insertHint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        insertHint.textColor = .tertiaryLabelColor

        // Cancel and Add stay at the top, beside the title, where they're seen.
        // Return and Esc work from the fields, not as window-wide keys, so
        // they don't take over the rest of the window (Find, for one).
        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel(_:)))
        cancel.controlSize = .small
        addButton.target = self
        addButton.action = #selector(accept(_:))
        addButton.controlSize = .small
        addButton.bezelColor = .controlAccentColor
        addButton.toolTip = "Add the primer (Return)"
        let header = NSStackView(views: [title, NSView(), cancel, addButton])
        header.spacing = 8

        let stack = NSStackView(views: [
            heading("NAME"), nameField,
            heading("DESCRIPTION"), descriptionField,
            heading("TAG"), colorRow,
            heading("SEQUENCE (5′ → 3′)"), textScroll, flipRow,
            info, warnings,
            heading("ANNEALING Tm"), tmRow,
            heading("PARTNER PRIMER"), partnerPopup,
            heading("CODON"), codonRow,
            heading("ENZYME SITE"), enzymeRow, padSwitch,
            heading("TAG OR PROTEASE SITE"), tagRow,
            insertHint,
        ])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        for v in [nameField, descriptionField, colorRow, flipRow, tmRow, partnerPopup, codonRow, padSwitch] as [NSView] {
            stack.setCustomSpacing(14, after: v)
        }
        stack.setCustomSpacing(18, after: warnings)
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 10, bottom: 12, right: 14)
        for v in [nameField, descriptionField, textScroll, info, warnings, tmRow, partnerPopup, codonRow, enzymeRow,
                  tagRow, insertHint] as [NSView] {
            v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -24).isActive = true
        }

        // The inputs scroll when the panel is short; the header stays put.
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
        for v in [header, scroll] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        NSLayoutConstraint.activate([
            header.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 10),
            header.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -14),
            header.topAnchor.constraint(equalTo: root.topAnchor, constant: 8),
            scroll.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            scroll.topAnchor.constraint(equalTo: header.bottomAnchor, constant: 6),
            scroll.bottomAnchor.constraint(equalTo: root.bottomAnchor),
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
                if !a.tm.isNaN {
                    line += " · Tm \(String(format: "%.1f", a.tm)) °C"
                    if typed.count > Int(s.length) { line += " annealing" }
                }
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
            checkFrames(site: s, tail: tail, lines: &lines, warnings: &warn)
            // With a partner the selection shows the product; alone, the site.
            var shown = (start: Int(s.start), length: Int(s.length))
            if let product = checkPartner(typed: typed, site: s, lines: &lines, warnings: &warn) {
                shown = product
            }
            oc_document_set_pending_primer(document.handle, primerName, typed, hexColor, &sites[0])
            if select, !document.hasSelection || document.selectionStart != shown.start
                || document.selectionLength != shown.length {
                document.select(start: shown.start, length: shown.length)
            }
            if select { selectionShown = shown }
        } else {
            oc_document_clear_pending_primer(document.handle)
            if !typed.isEmpty {
                warn.append(typed.count < 15 ? "Type at least 15 bases." : "It doesn't bind this sequence.")
            } else {
                lines.append("Type or paste a primer; the map and sequence show where it binds.")
            }
        }
        if !checkWarnings.isEmpty {
            // With a tail, say the warned Tm is the whole primer's, not the annealing part's.
            let tailed = site.map { typed.count > Int($0.length) } ?? false
            warn.append(contentsOf: checkWarnings.split(separator: "\n").map {
                tailed && $0.hasPrefix("Tm ") ? "Full-length " + String($0) : String($0)
            })
        }
        // The 3' end: its last five bases tinted, green with one to three G/C.
        if typed.count >= 5 {
            let clamp = Int(check.clamp_gc)
            let good = (1...3).contains(clamp)
            lines.append("Last 5 bases …\(typed.suffix(5).uppercased()): \(clamp) G/C" + (good ? " ✓" : ""))
            sequenceText.threePrimeTint = good ? .systemGreen : .systemOrange
        } else {
            sequenceText.threePrimeTint = nil
        }
        sequenceText.annealingLength = site.map { Int($0.length) } ?? 0
        info.stringValue = lines.joined(separator: "\n")
        info.textColor = typed.isEmpty ? .secondaryLabelColor : .labelColor
        warnings.stringValue = warn.joined(separator: "\n")
        warnings.isHidden = warn.isEmpty
        info.isHidden = lines.isEmpty
        addButton.isEnabled = site != nil
    }

    /// For each codon or tag in the tail: whether it lands in frame with the
    /// CDS the primer joins, and how to fix it when it doesn't.
    private func checkFrames(site s: OCPrimerSite, tail: Int, lines: inout [String], warnings: inout [String]) {
        var site = s
        for mark in sequenceText.marks where mark.part.kind != .enzymeSite && mark.start < tail {
            var frame = OCFrameCheck()
            oc_primer_part_frame(document.handle, &site, Int32(tail), Int32(mark.start), Int32(mark.length),
                                 mark.forward, &frame)
            guard frame.applies else { continue }
            let cds = withUnsafeBytes(of: frame.cds) { raw in
                String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
            }
            let name = mark.part.name
            if frame.against_cds {
                warnings.append("\(name) reads against \(cds); turn it round with the switch before inserting.")
            } else if frame.in_frame {
                lines.append("\(name) in frame with \(cds) ✓")
            } else {
                warnings.append("\(name) is out of frame with \(cds): add \(frame.fix) base\(frame.fix == 1 ? "" : "s") "
                                + "between it and the annealing part.")
            }
        }
    }

    /// With a partner chosen: the product, the gap between the annealing Tms
    /// and any dimer. Returns the product's span on the template, to select.
    private func checkPartner(typed: String, site s: OCPrimerSite, lines: inout [String],
                              warnings: inout [String]) -> (start: Int, length: Int)? {
        guard let index = partnerPopup.selectedItem?.representedObject as? Int,
              let partner = document.primers.first(where: { $0.index == index }), partner.length > 0 else { return nil }
        var mine = s
        var theirs = OCPrimerSite(start: Int32(partner.start), length: Int32(partner.length), strand: partner.strand,
                                  mismatches: 0)
        let product = Int(oc_pcr_product_length(document.handle, &mine, Int32(typed.count - Int(s.length)), &theirs,
                                                Int32(partner.bases.count - partner.length)))
        var a = OCPrimerCheck(), b = OCPrimerCheck()
        oc_string_free(oc_primer_check(String(typed.suffix(Int(s.length))), &a))
        oc_string_free(oc_primer_check(String(partner.bases.suffix(partner.length)), &b))
        var line = "With \(partner.name): "
        line += product > 0 ? "\(product.formatted()) bp product" : "no product (they don't face each other)"
        if !a.tm.isNaN, !b.tm.isNaN {
            let gap = abs(a.tm - b.tm)
            line += " · Tm gap \(String(format: "%.1f", gap)) °C"
            if gap > 5 { warnings.append("The Tms differ by more than 5 °C; fit this one to \(String(format: "%.0f", b.tm)) °C.") }
        }
        lines.append(line)
        let pair = takeString(oc_primer_pair_check(typed, partner.bases))
        if !pair.isEmpty { warnings.append(contentsOf: pair.split(separator: "\n").map { String($0) }) }
        guard product > 0 else { return nil }
        // The template stretch the product covers (tails aside).
        let forwardSite = s.strand == Int32(OC_STRAND_FORWARD) ? (Int(s.start), Int(s.length)) : (partner.start, partner.length)
        let reverseSite = s.strand == Int32(OC_STRAND_FORWARD) ? (partner.start, partner.length) : (Int(s.start), Int(s.length))
        var length = reverseSite.0 + reverseSite.1 - forwardSite.0
        if length <= 0 { length += document.length }
        return (forwardSite.0, length)
    }

    @objc private func partnerChanged(_ sender: Any?) { refresh() }

    @objc private func tmStepped(_ sender: Any?) {
        tmField.integerValue = tmStepper.integerValue
        fitToTm(nil)
    }

    @objc private func tmFieldChanged(_ sender: Any?) { fitToTm(nil) }

    /// The target Tm as typed, kept within the stepper's range.
    private func takeTargetTm() -> Int {
        let value = min(Int(tmStepper.maxValue), max(Int(tmStepper.minValue), tmField.integerValue))
        tmField.integerValue = value
        tmStepper.integerValue = value
        return value
    }

    /// Grows or shrinks the annealing part along the template to the target Tm.
    @objc private func fitToTm(_ sender: Any?) {
        let target = takeTargetTm()
        guard var s = site else { NSSound.beep(); return }
        sequenceText.removeWhitespace()
        let typed = bases
        var fitted = OCPrimerSite()
        let result = takeString(oc_primer_fit_tm(document.handle, typed, &s, Double(target), &fitted))
        guard fitted.length > 0, result.count >= Int(fitted.length) else { return }
        let total = (typed as NSString).length
        sequenceText.replaceBases(in: NSRange(location: total - Int(s.length), length: Int(s.length)),
                                  with: String(result.suffix(Int(fitted.length))))
        refresh()
    }

    private func nextPrimerName() -> String {
        let names = Set(document.primers.map { $0.name })
        var i = 1
        while names.contains("primer\(i)") { i += 1 }
        return "primer\(i)"
    }

    // MARK: Actions

    func textDidChange(_ notification: Notification) { refresh() }
    func controlTextDidChange(_ obj: Notification) { refresh() }

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

    /// Edit > Undo while the editor has the focus undoes typing in the field
    /// with the focus, never an edit to the document.
    @objc func undoEdit(_ sender: Any?) {
        guard let undo = focusedUndo, undo.canUndo else { NSSound.beep(); return }
        undo.undo()
    }

    @objc func redoEdit(_ sender: Any?) {
        guard let undo = focusedUndo, undo.canRedo else { NSSound.beep(); return }
        undo.redo()
    }

    /// The sequence box's own undo, or the name or description field's.
    private var focusedUndo: UndoManager? {
        (view.window?.firstResponder as? NSTextView)?.undoManager
    }

    @objc private func colorChanged(_ sender: Any?) {
        colorChosen = true
        refresh()
    }

    @objc private func flipChanged(_ sender: Any?) {
        // Keep the cursor on the same base of the flipped primer.
        let length = (sequenceText.string as NSString).length
        let caret = sequenceText.selectedRange()
        sequenceText.flip()
        noteFlipForUndo()
        sequenceText.setSelectedRange(NSRange(location: max(0, length - caret.location - caret.length),
                                              length: caret.length))
        refresh()
    }

    /// Undoing a flip in the box turns the switch back too (and redoing it, on).
    private func noteFlipForUndo() {
        sequenceText.undoManager?.registerUndo(withTarget: self) { editor in
            editor.flipSwitch.state = editor.flipSwitch.state == .on ? .off : .on
            editor.noteFlipForUndo()
        }
    }

    @objc private func aminoAcidChanged(_ sender: Any?) { fillCodons() }

    private func fillCodons() {
        codonPopup.removeAllItems()
        let aa = PrimerParts.aminoAcids[max(0, aminoAcidPopup.indexOfSelectedItem)]
        for (i, codon) in aa.codons.enumerated() {
            codonPopup.addItem(withTitle: i == 0 && aa.codons.count > 1 ? "\(codon) (E. coli)" : codon)
            codonPopup.lastItem?.representedObject = PrimerPart(kind: .codon, name: aa.shortName, bases: codon)
        }
    }

    @objc private func insertCodon(_ sender: Any?) { insert(codonPopup.selectedItem?.representedObject as? PrimerPart) }
    @objc private func insertEnzymeSite(_ sender: Any?) { insert(enzymePopup.selectedItem?.representedObject as? PrimerPart) }
    @objc private func insertTag(_ sender: Any?) { insert(tagPopup.selectedItem?.representedObject as? PrimerPart) }

    /// Puts a part in at the cursor, marked, as one undo step. With the
    /// switch on it goes in reverse complemented, so it still reads on the
    /// strand that was typed.
    private func insert(_ part: PrimerPart?) {
        guard let part = part else { NSSound.beep(); return }
        view.window?.makeFirstResponder(sequenceText)
        let atFivePrimeEnd = sequenceText.selectedRange().location == 0
        let pad = part.kind == .enzymeSite && atFivePrimeEnd && padSwitch.state == .on
        sequenceText.insertPart(part, forward: flipSwitch.state != .on, padding: pad ? PrimerParts.sitePadding : "")
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
        // Put back the selection the preview moved, unless it was changed since.
        if let shown = selectionShown, document.hasSelection, document.selectionStart == shown.start,
           document.selectionLength == shown.length {
            if selectionBefore.length > 0 {
                document.select(start: selectionBefore.start, length: selectionBefore.length)
            } else {
                oc_document_clear_selection(document.handle)
            }
        }
        close()
    }

    private func close() {
        // Typing in the name and description fields goes on the window's
        // undo; it means nothing once the editor is gone.
        view.window?.undoManager?.removeAllActions()
        Self.showing.removeAll { $0 === self }
        onClose?()
    }

    // MARK: Launch check

    static var isAnyOpen: Bool { !showing.isEmpty }
    /// Presses Add on each open editor.
    static func acceptAll() { showing.forEach { $0.accept(nil) } }
    /// Fits each open editor's primer to `tm` and returns where each now binds.
    static func fitAll(to tm: Int) -> [OCPrimerSite] {
        showing.compactMap {
            $0.tmStepper.integerValue = tm
            $0.tmStepped(nil)
            return $0.site
        }
    }
    /// Presses Cancel on each open editor.
    static func cancelAll() { showing.forEach { $0.cancel(nil) } }
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

/// Something inserted into a primer: a codon, an enzyme site, a tag or a
/// protease site.
final class PrimerPart: NSObject {
    enum Kind { case codon, enzymeSite, tag, proteaseSite }
    let kind: Kind
    let name: String    // shown on its mark: "Met", "EcoRI", "His6", "TEV"
    let bases: String   // as it reads, 5'->3'

    init(kind: Kind, name: String, bases: String) {
        self.kind = kind
        self.name = name
        self.bases = bases
    }

    var color: NSColor {
        switch kind {
        case .codon: return .systemGreen
        case .enzymeSite: return .systemPurple
        case .tag: return .systemBlue
        case .proteaseSite: return .systemOrange
        }
    }
}

/// One inserted part in the box: what it is and which way it reads there.
private final class PartMark: NSObject {
    let part: PrimerPart
    let forward: Bool

    init(part: PrimerPart, forward: Bool) {
        self.part = part
        self.forward = forward
    }
}

/// The primer's bases, with a line under each inserted part: its colour says
/// what kind it is, its arrow which way it reads, and its name sits under it.
/// The marks ride on the text as an attribute, so they move with edits.
final class PrimerTextView: NSTextView {
    private static let markKey = NSAttributedString.Key("RestrictoPrimerPart")
    private static let markFont = NSFont.systemFont(ofSize: 9, weight: .medium)
    private let baseFont = NSFont.monospacedSystemFont(ofSize: 13, weight: .regular)

    /// The bases' look, with room under each line for the marks.
    private var baseAttributes: [NSAttributedString.Key: Any] {
        let paragraph = NSMutableParagraphStyle()
        paragraph.lineSpacing = 16
        return [.font: baseFont, .foregroundColor: NSColor.textColor, .paragraphStyle: paragraph]
    }

    /// The box's own undo, apart from the document's and the window's, so it
    /// starts empty each time and ⌘Z here never reaches the document.
    private let ownUndo = UndoManager()
    override var undoManager: UndoManager? { ownUndo }

    /// The Tag colour well is for the primer, not these bases: with the box
    /// still focused, the colour panel would otherwise recolour its text.
    override func changeColor(_ sender: Any?) {}

    func setUpLook() {
        font = baseFont
        defaultParagraphStyle = baseAttributes[.paragraphStyle] as? NSParagraphStyle
        typingAttributes = baseAttributes
    }

    /// Typing never extends a part's mark.
    override var typingAttributes: [NSAttributedString.Key: Any] {
        get {
            var attributes = super.typingAttributes
            attributes[Self.markKey] = nil
            return attributes
        }
        set { super.typingAttributes = newValue }
    }

    /// Replaces the bases with unmarked ones.
    func setPlain(_ bases: String) {
        textStorage?.setAttributedString(NSAttributedString(string: bases, attributes: baseAttributes))
        ownUndo.removeAllActions()
        needsDisplay = true
    }

    /// Inserts a part at the cursor as one undo step, reading forward (as
    /// typed) or, reverse complemented, backward.
    func insertPart(_ part: PrimerPart, forward: Bool, padding: String = "") {
        let bases = (forward ? part.bases : PrimerParts.reverseComplement(part.bases)).uppercased()
        let range = selectedRange()
        guard shouldChangeText(in: range, replacementString: padding + bases) else { return }
        var attributes = baseAttributes
        attributes[Self.markKey] = PartMark(part: part, forward: forward)
        let text = NSMutableAttributedString(string: padding, attributes: baseAttributes)
        text.append(NSAttributedString(string: bases, attributes: attributes))
        textStorage?.replaceCharacters(in: range, with: text)
        didChangeText()
        setSelectedRange(NSRange(location: range.location + text.length, length: 0))
        needsDisplay = true
    }

    /// The marked parts, counted in bases from the 5' end (spaces and line
    /// breaks don't count).
    var marks: [(start: Int, length: Int, part: PrimerPart, forward: Bool)] {
        guard let storage = textStorage else { return [] }
        let text = storage.string as NSString
        var out: [(start: Int, length: Int, part: PrimerPart, forward: Bool)] = []
        storage.enumerateAttribute(Self.markKey, in: NSRange(location: 0, length: storage.length)) { value, range, _ in
            guard let mark = value as? PartMark else { return }
            func bases(_ r: NSRange) -> Int {
                (text.substring(with: r) as String).filter { !$0.isWhitespace }.count
            }
            out.append((bases(NSRange(location: 0, length: range.location)), bases(range), mark.part, mark.forward))
        }
        return out
    }

    /// Deletes spaces and line breaks, keeping the marks on their bases.
    func removeWhitespace() {
        guard let storage = textStorage else { return }
        let stripped = NSMutableAttributedString(attributedString: storage)
        let text = stripped.string as NSString
        var i = text.length - 1
        while i >= 0 {
            if let scalar = UnicodeScalar(text.character(at: i)), CharacterSet.whitespacesAndNewlines.contains(scalar) {
                stripped.deleteCharacters(in: NSRange(location: i, length: 1))
            }
            i -= 1
        }
        guard stripped.length != storage.length else { return }
        replaceAll(with: stripped)
    }

    /// Replaces all the text, as an undo step.
    private func replaceAll(with text: NSAttributedString) {
        guard let storage = textStorage else { return }
        let all = NSRange(location: 0, length: storage.length)
        guard shouldChangeText(in: all, replacementString: text.string) else { return }
        storage.replaceCharacters(in: all, with: text)
        didChangeText()
    }

    /// Replaces some bases with unmarked ones, as one undo step.
    func replaceBases(in range: NSRange, with bases: String) {
        guard shouldChangeText(in: range, replacementString: bases) else { return }
        textStorage?.replaceCharacters(in: range, with: NSAttributedString(string: bases, attributes: baseAttributes))
        didChangeText()
        setSelectedRange(NSRange(location: 0, length: 0))
        needsDisplay = true
    }

    /// How many bases at the 3' end anneal; a grey line marks them.
    var annealingLength = 0 {
        didSet { if annealingLength != oldValue { needsDisplay = true } }
    }

    /// The tint behind the last five bases (the 3' end), or nil for none.
    var threePrimeTint: NSColor? {
        didSet { applyThreePrimeTint() }
    }

    private func applyThreePrimeTint() {
        guard let layout = layoutManager, let storage = textStorage else { return }
        let all = NSRange(location: 0, length: storage.length)
        layout.removeTemporaryAttribute(.backgroundColor, forCharacterRange: all)
        guard let tint = threePrimeTint, let end = lastBases(5) else { return }
        layout.addTemporaryAttribute(.backgroundColor, value: tint.withAlphaComponent(0.22), forCharacterRange: end)
    }

    /// The character range of the last `count` bases (skipping spaces), or nil if there are fewer.
    private func lastBases(_ count: Int) -> NSRange? {
        guard let storage = textStorage, count > 0 else { return nil }
        let text = storage.string as NSString
        var i = text.length, found = 0
        while i > 0, found < count {
            i -= 1
            if let scalar = UnicodeScalar(text.character(at: i)), !CharacterSet.whitespacesAndNewlines.contains(scalar) {
                found += 1
            }
        }
        return found == count ? NSRange(location: i, length: text.length - i) : nil
    }

    /// Reverse complements the bases, keeping each mark on its bases with its
    /// arrow turned round.
    func flip() {
        guard let storage = textStorage else { return }
        let length = storage.length
        var marks: [(NSRange, PartMark)] = []
        storage.enumerateAttribute(Self.markKey, in: NSRange(location: 0, length: length)) { value, range, _ in
            if let mark = value as? PartMark { marks.append((range, mark)) }
        }
        let flipped = NSMutableAttributedString(string: PrimerParts.reverseComplement(storage.string),
                                                attributes: baseAttributes)
        for (range, mark) in marks {
            flipped.addAttribute(Self.markKey, value: PartMark(part: mark.part, forward: !mark.forward),
                                 range: NSRange(location: length - range.location - range.length, length: range.length))
        }
        replaceAll(with: flipped)
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard let storage = textStorage else { return }
        // The annealing part: a grey line along the bases that bind, named at
        // its 3' end so a part that runs into it keeps its own name clear.
        // Drawn first, so a part's line shows over it.
        if annealingLength > 0, let range = lastBases(annealingLength) {
            drawMark(range, color: .tertiaryLabelColor, name: "anneals", forward: true, nameAtEnd: true)
        }
        storage.enumerateAttribute(Self.markKey, in: NSRange(location: 0, length: storage.length)) { value, range, _ in
            guard let mark = value as? PartMark else { return }
            drawMark(range, color: mark.part.color, name: mark.part.name, forward: mark.forward)
        }
    }

    /// A line under the characters in `range`, with an arrowhead the way it
    /// reads and `name` under its start (or its end).
    private func drawMark(_ range: NSRange, color: NSColor, name: String, forward: Bool, nameAtEnd: Bool = false) {
        guard let layout = layoutManager, let container = textContainer else { return }
        let origin = textContainerOrigin
        let descent = abs(baseFont.descender)
        let glyphs = layout.glyphRange(forCharacterRange: range, actualCharacterRange: nil)
        var first = true
        layout.enumerateLineFragments(forGlyphRange: glyphs) { lineRect, _, _, lineGlyphs, _ in
            let piece = NSIntersectionRange(glyphs, lineGlyphs)
            guard piece.length > 0 else { return }
            let box = layout.boundingRect(forGlyphRange: piece, in: container)
            let baseline = origin.y + lineRect.minY + layout.location(forGlyphAt: piece.location).y
            let y = baseline + descent + 3
            let x0 = origin.x + box.minX + 1, x1 = origin.x + box.maxX - 1
            color.setStroke()
            color.setFill()
            let line = NSBezierPath()
            line.lineWidth = 1.5
            line.move(to: NSPoint(x: x0, y: y))
            line.line(to: NSPoint(x: x1, y: y))
            line.stroke()
            // The arrowhead where the part ends, the way it reads.
            let lastPiece = NSMaxRange(piece) == NSMaxRange(glyphs)
            if forward ? lastPiece : first {
                let tip = forward ? x1 : x0, back: CGFloat = forward ? -5 : 5
                let head = NSBezierPath()
                head.move(to: NSPoint(x: tip, y: y))
                head.line(to: NSPoint(x: tip + back, y: y - 3))
                head.line(to: NSPoint(x: tip + back, y: y + 3))
                head.close()
                head.fill()
            }
            let label = NSAttributedString(string: name, attributes: [.font: Self.markFont, .foregroundColor: color])
            if nameAtEnd ? lastPiece : first {
                label.draw(at: NSPoint(x: nameAtEnd ? max(x0, x1 - label.size().width) : x0, y: y + 2))
            }
            first = false
        }
    }
}