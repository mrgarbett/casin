// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import RestrictoCore

/// Double-strand sequence view. Lives inside an NSScrollView as its document
/// view; only the visible lines are drawn.
final class SequenceView: NSView {
    private let document: CoreDocument
    private var layout: OpaquePointer?
    private var layoutWidth = 0.0
    private var layoutDark = false
    private var dragging = false
    private var anchor = 0
    /// A drag that started on an amino acid: the codon it started on, so the
    /// selection grows codon by codon while it stays on amino acids.
    private var codonAnchor: Int?
    /// The codon whose amino acid is under the mouse, outlined on the strands.
    private var hoverCodon = -1 {
        didSet {
            guard hoverCodon != oldValue else { return }
            if let layout = layout { oc_seq_layout_set_hover_codon(layout, Int32(hoverCodon)) }
            needsDisplay = true
        }
    }

    var showComplement = true {
        didSet { invalidateLayout() }
    }

    init(document: CoreDocument) {
        self.document = document
        super.init(frame: NSRect(x: 0, y: 0, width: 600, height: 400))
        document.observe { [weak self] what in
            guard let self = self else { return }
            if what & (Change.sequence | Change.enzymes) != 0 { self.invalidateLayout() }
            if what & Change.selection != 0, !self.dragging, self.document.hasSelection {
                self.scrollToPosition(self.document.selectionStart)
            }
            self.needsDisplay = true
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    deinit {
        if let layout = layout { oc_seq_layout_free(layout) }
    }

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    func invalidateLayout() {
        if let layout = layout { oc_seq_layout_free(layout) }
        layout = nil
        updateSize()
        needsDisplay = true
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        invalidateLayout()
    }

    /// Room kept free along the right edge (the panel's pill floats there).
    var trailingInset: CGFloat = 0 {
        didSet { if trailingInset != oldValue { invalidateLayout() } }
    }

    /// An event's point in the layout's coordinates.
    private func layoutPoint(_ event: NSEvent) -> NSPoint {
        convert(event.locationInWindow, from: nil)
    }

    private var availableWidth: CGFloat {
        enclosingScrollView?.contentSize.width ?? bounds.width
    }

    @discardableResult
    private func ensureLayout() -> OpaquePointer? {
        let width = Double(max(1, availableWidth - trailingInset))
        let dark = isDarkAppearance
        if layout == nil || layoutWidth != width || layoutDark != dark {
            if let old = layout { oc_seq_layout_free(old) }
            // The bottom-pill toggles and map options apply here as on the map.
            let hidden = document.mapHiddenTypes.sorted().joined(separator: ",")
            layout = hidden.withCString { types in
                var options = OCSeqOptions(frames: Int32(document.frames), group_tens: document.groupTens,
                                           show_enzymes: document.mapShowsEnzymes, show_features: true,
                                           show_primers: document.mapShowsPrimers, hidden_types: types)
                return oc_seq_layout_new_options(document.handle, width, showComplement, &options, dark,
                                                 measureText, nil)
            }
            layoutWidth = width
            layoutDark = dark
            if let layout = layout { oc_seq_layout_set_hover_codon(layout, Int32(hoverCodon)) }
        }
        return layout
    }

    /// Matches the scroll view's width and the content height.
    func updateSize() {
        guard let layout = ensureLayout() else { return }
        let visibleHeight = enclosingScrollView?.contentSize.height ?? 0
        let size = NSSize(width: availableWidth, height: max(oc_seq_layout_total_height(layout) + 20, visibleHeight))
        if frame.size != size { setFrameSize(size) }
    }

    func scrollToPosition(_ position: Int) {
        guard let layout = ensureLayout() else { return }
        let top = oc_seq_layout_line_top(layout, Int32(position))
        let height = oc_seq_layout_line_height(layout, Int32(position))
        let target = NSRect(x: 0, y: top, width: 1, height: height)
        if !visibleRect.contains(target) {
            scroll(NSPoint(x: 0, y: max(0, top - 20)))
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext, let layout = ensureLayout(),
              let scene = oc_seq_layout_render(layout, document.handle, dirtyRect.minY, dirtyRect.height) else { return }
        defer { oc_scene_free(scene) }
        ctx.saveGState()
        ctx.translateBy(x: 0, y: dirtyRect.minY)   // scenes are relative to the scroll offset
        drawScene(scene, in: ctx, trailing: trailingInset)
        ctx.restoreGState()
    }

    // MARK: Keyboard: typing bases edits the sequence

    override var acceptsFirstResponder: Bool { true }

    override func keyDown(with event: NSEvent) {
        guard !document.isEmpty else { super.keyDown(with: event); return }
        let flags = event.modifierFlags.intersection([.command, .control, .option])
        guard flags.isEmpty, let chars = event.charactersIgnoringModifiers, !chars.isEmpty else {
            super.keyDown(with: event)
            return
        }
        switch chars.unicodeScalars.first?.value ?? 0 {
        case 0x7F:   // Delete
            if document.hasSelection && document.selectionLength > 0 {
                oc_document_delete_selection(document.handle)
            } else {
                oc_document_delete_backward(document.handle)
            }
        case UInt32(NSDeleteFunctionKey):
            if document.hasSelection && document.selectionLength > 0 {
                oc_document_delete_selection(document.handle)
            } else {
                oc_document_delete_forward(document.handle)
            }
        default:
            // Letters only; anything that isn't a base beeps.
            if chars.unicodeScalars.allSatisfy({ CharacterSet.letters.contains($0) }) {
                if !oc_document_type_bases(document.handle, chars) { NSSound.beep() }
            } else {
                super.keyDown(with: event)
            }
        }
    }

    @objc func paste(_ sender: Any?) {
        guard let text = NSPasteboard.general.string(forType: .string), !document.isEmpty else { return }
        if !oc_document_type_bases(document.handle, text) { NSSound.beep() }
    }

    @objc func cut(_ sender: Any?) {
        let bases = document.selectedBases
        guard !bases.isEmpty else { NSSound.beep(); return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(bases, forType: .string)
        oc_document_delete_selection(document.handle)
    }

    @objc func delete(_ sender: Any?) {
        guard document.hasSelection && document.selectionLength > 0 else { NSSound.beep(); return }
        oc_document_delete_selection(document.handle)
    }

    override func selectAll(_ sender: Any?) {
        guard !document.isEmpty else { NSSound.beep(); return }
        document.select(start: 0, length: document.length)
    }

    // MARK: Mouse

    override func mouseDown(with event: NSEvent) {
        guard let layout = ensureLayout(), !document.isEmpty else { return }
        let p = layoutPoint(event)
        if event.modifierFlags.contains(.shift) {
            if !document.hasSelection { anchor = 0 }
            dragging = true
            extendSelection(to: p)
            SelectionBadge.shared.show(for: document, event: event, in: self)
            return
        }
        // An amino acid in a translation row selects its codon; dragging on
        // across amino acids selects their codons too.
        let codon = Int(oc_seq_layout_codon_at(layout, p.x, p.y))
        if codon >= 0 {
            window?.makeFirstResponder(self)
            dragging = true
            codonAnchor = codon
            anchor = codon
            document.select(start: codon, length: 3)
            SelectionBadge.shared.show(for: document, event: event, in: self)
            return
        }
        let site = Int(oc_seq_layout_site_at(layout, p.x, p.y))
        if site >= 0 { document.activateSite(site); return }
        let feature = Int(oc_seq_layout_feature_at(layout, p.x, p.y))
        if feature >= 0 { document.activateFeature(feature); return }
        window?.makeFirstResponder(self)
        dragging = true
        anchor = Int(oc_seq_layout_position_at(layout, p.x, p.y))
        document.select(start: anchor, length: 0)
    }

    override func mouseDragged(with event: NSEvent) {
        guard dragging else { return }
        autoscroll(with: event)
        extendSelection(to: layoutPoint(event))
        SelectionBadge.shared.show(for: document, event: event, in: self)
    }

    override func mouseUp(with event: NSEvent) {
        if dragging { SelectionBadge.shared.hideSoon() }
        dragging = false
        codonAnchor = nil
    }

    private func extendSelection(to p: NSPoint) {
        guard let layout = layout else { return }
        let y = min(max(p.y, visibleRect.minY), visibleRect.maxY - 1)
        if let first = codonAnchor {
            // From the first codon to the one under the mouse, both whole;
            // off the amino acids, to the nearest base.
            let codon = Int(oc_seq_layout_codon_at(layout, p.x, y))
            hoverCodon = codon
            let end: Int
            if codon >= 0 {
                end = codon >= first ? codon + 3 : codon
            } else {
                end = Int(oc_seq_layout_position_at(layout, p.x, y))
            }
            let start = min(first, end), stop = max(first + 3, end)
            document.select(start: start, length: min(stop, document.length) - start)
            return
        }
        let pos = Int(oc_seq_layout_position_at(layout, p.x, y))
        document.select(start: min(pos, anchor), length: abs(pos - anchor))
    }

    // MARK: Hover: codons and tooltips

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero,
                                       options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func mouseMoved(with event: NSEvent) {
        guard let layout = layout else { return }
        let p = layoutPoint(event)
        // An amino acid under the mouse outlines its codon.
        hoverCodon = Int(oc_seq_layout_codon_at(layout, p.x, p.y))
        let tip = takeString(oc_seq_layout_tooltip(layout, p.x, p.y))
        toolTip = tip.isEmpty ? nil : tip
    }

    override func mouseExited(with event: NSEvent) {
        hoverCodon = -1
    }
}
