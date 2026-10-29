// SPDX-License-Identifier: GPL-3.0-or-later
// The Add Primer wizard: a window with the primer's name, colour and bases in
// a sidebar, and in the middle a live preview of where it binds: the map and
// the sequence with the primer drawn over it. Add puts the primer on the
// document as one undo step.
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
    private let colorWell = NSColorWell(style: .minimal)
    private var colorChosen = false
    private let sequenceText = NSTextView()
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
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 640),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.minSize = NSSize(width: 760, height: 480)
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
        window.setContentSize(NSSize(width: 1000, height: 640))
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

        let flip = NSButton(title: "Reverse Complement", target: self, action: #selector(reverseComplement(_:)))
        flip.controlSize = .small
        flip.toolTip = "Turn the bases into the other strand's primer"

        info.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        info.textColor = .labelColor
        warnings.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        warnings.textColor = .systemOrange

        let colorRow = NSStackView(views: [colorWell, NSTextField(labelWithString: "Colour")])
        colorRow.spacing = 8

        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancel(_:)))
        cancel.keyEquivalent = "\u{1b}"
        addButton.target = self
        addButton.action = #selector(accept(_:))
        addButton.keyEquivalent = "\r"
        addButton.bezelColor = .controlAccentColor
        let buttons = NSStackView(views: [NSView(), cancel, addButton])
        buttons.spacing = 8

        let stack = NSStackView(views: [
            heading("NAME"), nameField,
            heading("TAG"), colorRow,
            heading("SEQUENCE (5′ → 3′)"), textScroll, flip,
            info, warnings,
            NSView(), buttons,
        ])
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 6
        stack.setCustomSpacing(14, after: nameField)
        stack.setCustomSpacing(14, after: colorRow)
        stack.setCustomSpacing(14, after: flip)
        stack.edgeInsets = NSEdgeInsets(top: 12, left: 16, bottom: 16, right: 16)
        for v in [nameField, textScroll, info, warnings, buttons] as [NSView] {
            v.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -32).isActive = true
        }
        stack.setHuggingPriority(.defaultLow, for: .vertical)

        let root = NSView()
        stack.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            stack.topAnchor.constraint(equalTo: root.safeAreaLayoutGuide.topAnchor),
            stack.bottomAnchor.constraint(equalTo: root.bottomAnchor),
        ])
        let controller = NSViewController()
        controller.view = root
        return controller
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
            oc_document_add_primer_at(preview.handle, primerName, typed, hexColor, &sites[0])
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

    @objc private func reverseComplement(_ sender: Any?) {
        let pairs: [Character: Character] = ["A": "T", "T": "A", "G": "C", "C": "G", "a": "t", "t": "a",
                                             "g": "c", "c": "g", "N": "N", "n": "n"]
        let flipped = String(bases.reversed().map { pairs[$0] ?? $0 })
        sequenceText.string = flipped
        refresh()
    }

    @objc private func accept(_ sender: Any?) {
        guard var s = site else { NSSound.beep(); return }
        oc_document_add_primer_at(target.handle, primerName, bases, hexColor, &s)
        target.select(start: Int(s.start), length: Int(s.length))
        close()
    }

    @objc private func cancel(_ sender: Any?) { close() }

    /// For the launch check: the open wizards' windows, and pressing Add on each.
    static var openWindows: [NSWindow] { openWizards.compactMap { $0.window } }
    static func acceptAll() { openWizards.forEach { $0.accept(nil) } }

    func windowWillClose(_ notification: Notification) {
        Self.openWizards.removeAll { $0 === self }
    }
}

extension PrimerWizard: NSTextFieldDelegate {
    func controlTextDidChange(_ obj: Notification) { refresh() }
}
