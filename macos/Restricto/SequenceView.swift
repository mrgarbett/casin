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

    private var availableWidth: CGFloat {
        enclosingScrollView?.contentSize.width ?? bounds.width
    }

    @discardableResult
    private func ensureLayout() -> OpaquePointer? {
        let width = Double(availableWidth)
        let dark = isDarkAppearance
        if layout == nil || layoutWidth != width || layoutDark != dark {
            if let old = layout { oc_seq_layout_free(old) }
            layout = oc_seq_layout_new_frames(document.handle, width, showComplement, Int32(document.frames), dark,
                                              measureText, nil)
            layoutWidth = width
            layoutDark = dark
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
        drawScene(scene, in: ctx)
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

    // MARK: Mouse

    override func mouseDown(with event: NSEvent) {
        guard let layout = ensureLayout(), !document.isEmpty else { return }
        let p = convert(event.locationInWindow, from: nil)
        if event.modifierFlags.contains(.shift) {
            if !document.hasSelection { anchor = 0 }
            dragging = true
            extendSelection(to: p)
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
        extendSelection(to: convert(event.locationInWindow, from: nil))
    }

    override func mouseUp(with event: NSEvent) {
        dragging = false
    }

    private func extendSelection(to p: NSPoint) {
        guard let layout = layout else { return }
        let y = min(max(p.y, visibleRect.minY), visibleRect.maxY - 1)
        let pos = Int(oc_seq_layout_position_at(layout, p.x, y))
        document.select(start: min(pos, anchor), length: abs(pos - anchor))
    }

    // MARK: Tooltips

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func mouseMoved(with event: NSEvent) {
        guard let layout = layout else { return }
        let p = convert(event.locationInWindow, from: nil)
        let tip = takeString(oc_seq_layout_tooltip(layout, p.x, p.y))
        toolTip = tip.isEmpty ? nil : tip
    }
}
