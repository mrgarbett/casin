// SPDX-License-Identifier: GPL-3.0-or-later
// The Add Primer wizard: a window with the primer's name, description, colour
// and bases in a sidebar, with codons, enzyme sites and tags to insert into it,
// and in the middle a live preview of where it binds: the map and the sequence
// with the primer drawn over it. Add puts the primer on the document as one
// undo step.
import AppKit
import RestrictoCore

final class PrimerWizard: NSWindowController, NSWindowDelegate, NSTextViewDelegate {
    /// Open wizards, kept alive until their windows close.
    private static var openWizards: [PrimerWizard] = []

    /// Opens a wizard for `target`, filled in with `bases` (5'->3') if given.
    static func show(for target: CoreDocument, bases: String = "", near parent: NSWindow?) {
        let wizard = PrimerWizard(target: target, bases: bases)
        openWizards.append(wizard)
        if let parent = parent, let window = wizard.window {
            // Centred on the document window, a little lower so the title stays visible.
            let p = parent.frame, w = window.frame
            window.setFrameOrigin(NSPoint(x: p.midX - w.width / 2, y: max(p.minY, p.midY - w.height / 2 - 20)))
        } else {
            wizard.window?.center()
        }
        wizard.showWindow(nil)
        wizard.window?.makeFirstResponder(wizard.sequenceText)
    }

    private let target: CoreDocument
    /// A copy of the document the primer is tried on; the preview shows it.
    private let preview: CoreDocument
    private var previewHasPrimer = false
    private var site: OCPrimerSite?

    private let nameField = NSTextField(string: "")
    private let descriptionField = NSTextField(string: "")
    private let colorWell = NSColorWell(style: .minimal)
    private var colorChosen = false
    private let sequenceText = NSTextView()
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
    private let addButton = NSButton(title: "Add Primer", target: nil, action: nil)

    private let mapView: MapView
    private let sequenceView: SequenceView
    private let sequenceScroll = NSScrollView()
    private let previewSplit = NSSplitView()
    private var splitPositioned = false
    private let emptyState = NSTextField(labelWithString: "Type or paste a primer to see where it binds.")

    private init(target: CoreDocument, bases: String) {
        self.target = target
        let copy = CoreDocument()
        oc_document_copy(target.handle, copy.handle)
        copy.mapShowsEnzymes = false
        let seqView = SequenceView(document: copy)
        seqView.isEditable = false
        preview = copy
        mapView = MapView(document: copy)
        sequenceView = seqView
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1040, height: 720),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.minSize = NSSize(width: 800, height: 520)
        window.title = "Add Primer"
        window.subtitle = target.name.isEmpty ? "Untitled" : target.name
        window.toolbarStyle = .unified
        window.toolbar = NSToolbar(identifier: "PrimerWizard")
        window.isReleasedWhenClosed = false
        window.tabbingMode = .disallowed
        super.init(window: window)
        window.delegate = self
        buildContent()
        sequenceText.string = bases
        // A 5' tail is what gets added most, so start at the 5' end.
        sequenceText.setSelectedRange(NSRange(location: 0, length: 0))
        refresh()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    // MARK: Layout

    private func buildContent() {
        guard let window = window else { return }
        let split = NSSplitViewController()
        let sidebar = NSSplitViewItem(sidebarWithViewController: sidebarController())
        sidebar.minimumThickness = 280
        sidebar.maximumThickness = 360
        sidebar.canCollapse = false
        split.addSplitViewItem(sidebar)
        let main = NSViewController()
        main.view = previewView()
        split.addSplitViewItem(NSSplitViewItem(viewController: main))
        window.contentViewController = split
        window.setContentSize(NSSize(width: 1040, height: 720))
    }

    private func sidebarController() -> NSViewController {
        func heading(_ text: String) -> NSTextField {
            let label = NSTextField(labelWithString: text)
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
            label.textColor = .secondaryLabelColor
            return label
        }
        nameField.placeholderString = "primer"
        nameField.stringValue = nextPrimerName()
        nameField.delegate = self

        colorWell.color = NSColor(srgbRed: 0xe6 / 255.0, green: 0x61 / 255.0, blue: 0, alpha: 1)
        colorWell.target = self
        colorWell.action = #selector(colorChanged(_:))
        colorWell.toolTip = "The primer's colour on the map and sequence"

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
        textScroll.heightAnchor.constraint(equalToConstant: 110).isActive = true

        descriptionField.placeholderString = "Optional"
        descriptionField.delegate = self

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

        let colorRow = NSStackView(views: [colorWell, NSTextField(labelWithString: "Colour")])
        colorRow.spacing = 8

        // Codon: an amino acid, then one of its codons.
        for aa in PrimerParts.aminoAcids { aminoAcidPopup.addItem(withTitle: aa.name) }
        aminoAcidPopup.target = self
        aminoAcidPopup.action = #selector(aminoAcidChanged(_:))
        fillCodons()
        let codonRow = insertRow([aminoAcidPopup, codonPopup], action: #selector(insertCodon(_:)))
        codonPopup.widthAnchor.constraint(equalToConstant: 108).isActive = true

        // Enzyme site: the unambiguous sites, with how often each cuts here.
        for e in target.enzymes.filter({ $0.site.allSatisfy { "ACGT".contains($0) } })
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

        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel(_:)))
        cancel.keyEquivalent = "\u{1b}"
        addButton.target = self
        addButton.action = #selector(accept(_:))
        addButton.keyEquivalent = "\r"
        addButton.bezelColor = .controlAccentColor
        let buttons = NSStackView(views: [NSView(), cancel, addButton])
        buttons.spacing = 8

        let insertHint = NSTextField(wrappingLabelWithString: "Inserts go in at the cursor, in lowercase.")
        insertHint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        insertHint.textColor = .tertiaryLabelColor

        let stack = NSStackView(views: [
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
        for v in [nameField, descriptionField, colorRow, flipRow, codonRow, enzymeRow] as [NSView] {
            stack.setCustomSpacing(14, after: v)
        }
        stack.setCustomSpacing(18, after: warnings)
        stack.edgeInsets = NSEdgeInsets(top: 12, left: 16, bottom: 12, right: 16)
        for v in [nameField, descriptionField, textScroll, info, warnings, codonRow, enzymeRow, tagRow,
                  insertHint] as [NSView] {
            v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -32).isActive = true
        }

        // The inputs scroll when the window is short; the buttons stay put.
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
            scroll.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: buttons.topAnchor, constant: -10),
            buttons.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 16),
            buttons.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -16),
            buttons.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -16),
        ])
        let controller = NSViewController()
        controller.view = root
        return controller
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

    private func previewView() -> NSView {
        sequenceScroll.documentView = sequenceView
        sequenceScroll.hasVerticalScroller = true
        sequenceScroll.autohidesScrollers = true
        sequenceScroll.drawsBackground = false
        sequenceScroll.contentView.postsFrameChangedNotifications = true
        NotificationCenter.default.addObserver(forName: NSView.frameDidChangeNotification,
                                               object: sequenceScroll.contentView, queue: .main) { [weak self] _ in
            self?.sequenceView.updateSize()
        }
        // The map above the sequence where the primer binds.
        previewSplit.isVertical = false
        previewSplit.dividerStyle = .thin
        previewSplit.addArrangedSubview(mapView)
        previewSplit.addArrangedSubview(sequenceScroll)
        previewSplit.setHoldingPriority(.defaultLow - 1, forSubviewAt: 0)

        emptyState.textColor = .secondaryLabelColor
        emptyState.font = .systemFont(ofSize: 15)
        emptyState.alignment = .center

        let root = NSView()
        for v in [previewSplit, emptyState] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(v)
        }
        NSLayoutConstraint.activate([
            previewSplit.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            previewSplit.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            previewSplit.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor),
            previewSplit.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            emptyState.centerXAnchor.constraint(equalTo: root.centerXAnchor),
            emptyState.centerYAnchor.constraint(equalTo: root.centerYAnchor),
            emptyState.widthAnchor.constraint(lessThanOrEqualTo: root.widthAnchor, constant: -40),
        ])
        return root
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

    /// Tries the primer on the copy and shows the result.
    private func refresh() {
        if previewHasPrimer {
            oc_document_undo(preview.handle)
            previewHasPrimer = false
        }
        let typed = bases
        var sites = [OCPrimerSite](repeating: OCPrimerSite(), count: 8)
        let count = typed.isEmpty ? 0 : Int(oc_primer_sites(target.handle, typed, &sites, Int32(sites.count)))
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
            oc_document_add_primer_at(preview.handle, primerName, typed, hexColor, nil, &sites[0])
            previewHasPrimer = true
            preview.select(start: Int(s.start), length: Int(s.length))
            sequenceView.scrollToPosition(Int(s.start))
        } else if !typed.isEmpty {
            warn.append(typed.count < 15 ? "Type at least 15 bases." : "It doesn't bind this sequence.")
            preview.select(start: 0, length: 0)
        }
        if !checkWarnings.isEmpty {
            warn.append(contentsOf: checkWarnings.split(separator: "\n").map { String($0) })
        }
        info.stringValue = lines.joined(separator: "\n")
        warnings.stringValue = warn.joined(separator: "\n")
        warnings.isHidden = warn.isEmpty
        info.isHidden = lines.isEmpty
        let binds = site != nil
        previewSplit.isHidden = !binds
        if binds && !splitPositioned {
            // The map takes a little over half the height the first time it shows.
            splitPositioned = true
            DispatchQueue.main.async { [weak self] in
                guard let self = self else { return }
                self.previewSplit.setPosition(self.previewSplit.bounds.height * 0.55, ofDividerAt: 0)
            }
        }
        emptyState.isHidden = binds
        if !binds {
            emptyState.stringValue = typed.isEmpty ? "Type or paste a primer to see where it binds."
                                                   : "No binding site yet."
        }
        addButton.isEnabled = binds
    }

    private func nextPrimerName() -> String {
        let names = Set(target.primers.map { $0.name })
        var i = names.count + 1
        while names.contains("primer\(i)") { i += 1 }
        return "primer\(i)"
    }

    // MARK: Actions

    func textDidChange(_ notification: Notification) { refresh() }

    @objc private func colorChanged(_ sender: Any?) {
        colorChosen = true
        refresh()
    }

    @objc private func flipChanged(_ sender: Any?) {
        // Keep the cursor on the same base of the flipped primer.
        let text = sequenceText.string
        let caret = sequenceText.selectedRange()
        let flipped = PrimerParts.reverseComplement(text)
        sequenceText.string = flipped
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
        window?.makeFirstResponder(sequenceText)
        let range = sequenceText.selectedRange()
        sequenceText.insertText(text.lowercased(), replacementRange: range)
    }

    @objc private func accept(_ sender: Any?) {
        guard var s = site else { NSSound.beep(); return }
        let description = descriptionField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        oc_document_add_primer_at(target.handle, primerName, bases, hexColor, description, &s)
        target.select(start: Int(s.start), length: Int(s.length))
        close()
    }

    @objc private func cancel(_ sender: Any?) { close() }

    /// For the launch check: the open wizards' windows, and pressing Add on each.
    static var openWindows: [NSWindow] { openWizards.compactMap { $0.window } }
    static func acceptAll() { openWizards.forEach { $0.accept(nil) } }
    /// Inserts the first tag (His6) at each wizard's cursor.
    static func insertFirstTag() {
        openWizards.forEach {
            $0.tagPopup.selectItem(at: 0)
            $0.insertTag(nil)
        }
    }

    func windowWillClose(_ notification: Notification) {
        Self.openWizards.removeAll { $0 === self }
    }
}

extension PrimerWizard: NSTextFieldDelegate {
    func controlTextDidChange(_ obj: Notification) { refresh() }
}

/// Top-down coordinates, so the sidebar's inputs start at the top of their scroll view.
private final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}
