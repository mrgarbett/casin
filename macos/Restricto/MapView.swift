// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import RestrictoCore

/// Circular or linear map, drawn from the shared MapLayout.
final class MapView: NSView {
    private let document: CoreDocument
    private var layout: OpaquePointer?
    private var layoutSize = CGSize.zero
    private var layoutDark = false
    // drag state
    private var selecting = false
    private var anchor = 0
    private var lastAngle = 0.0
    private var accumulated = 0.0
    /// Told the base under the mouse on the backbone, and -1 when it leaves.
    var onHoverBase: ((Int) -> Void)?
    /// Called with a feature's index when it is double-clicked.
    var onEditFeature: ((Int) -> Void)?

    init(document: CoreDocument) {
        self.document = document
        super.init(frame: NSRect(x: 0, y: 0, width: 520, height: 520))
        document.observe { [weak self] what in
            guard let self = self else { return }
            if what & (Change.sequence | Change.enzymes | Change.pending) != 0 { self.invalidateLayout() }
            self.needsDisplay = true
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    deinit {
        if let layout = layout { oc_map_layout_free(layout) }
    }

    /// Room kept free along the right edge (the panel's pill floats there).
    var trailingInset: CGFloat = 0 {
        didSet { if trailingInset != oldValue { invalidateLayout() } }
    }

    /// Room kept free along the bottom (the status pill floats there when
    /// the map is stacked over the sequence).
    var bottomInset: CGFloat = 0 {
        didSet { if bottomInset != oldValue { invalidateLayout() } }
    }

    /// The size the map is laid out in: the view less the insets.
    private var mapSize: CGSize {
        CGSize(width: max(1, bounds.width - trailingInset), height: max(1, bounds.height - bottomInset))
    }

    /// An event's point in the map's own coordinates.
    private func mapPoint(_ event: NSEvent) -> NSPoint {
        convert(event.locationInWindow, from: nil)
    }

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    func invalidateLayout() {
        if let layout = layout { oc_map_layout_free(layout) }
        layout = nil
        needsDisplay = true
    }

    override func viewDidChangeEffectiveAppearance() {
        super.viewDidChangeEffectiveAppearance()
        invalidateLayout()
    }

    private func ensureLayout() -> OpaquePointer? {
        let dark = isDarkAppearance
        // Laid out afresh at every size, including each frame of a side
        // panel sliding open: it takes well under a millisecond, and the map
        // then ends exactly where it will stay, with no snap at the end.
        if layout == nil || layoutSize != mapSize || layoutDark != dark {
            if let old = layout { oc_map_layout_free(old) }
            layout = newMapLayout(document, size: mapSize, dark: dark)
            layoutSize = mapSize
            layoutDark = dark
        }
        return layout
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext, let layout = ensureLayout(),
              let scene = oc_map_layout_render(layout, document.handle) else { return }
        defer { oc_scene_free(scene) }
        drawScene(scene, in: ctx, trailing: trailingInset)
    }

    // MARK: Mouse

    override func mouseDown(with event: NSEvent) {
        guard let layout = ensureLayout(), !document.isEmpty else { return }
        let p = mapPoint(event)
        var kind: Int32 = 0, index: Int32 = 0, position: Int32 = 0
        oc_map_layout_hit(layout, p.x, p.y, &kind, &index, &position)
        selecting = false
        switch kind {
        case Int32(OC_HIT_FEATURE):
            document.activateFeature(Int(index))
            if event.clickCount == 2 { onEditFeature?(Int(index)) }
        case Int32(OC_HIT_SITE):
            document.activateSite(Int(index))
        case Int32(OC_HIT_SEQUENCE):
            selecting = true
            anchor = Int(position)
            lastAngle = oc_map_layout_angle_at(layout, p.x, p.y)
            accumulated = 0
            document.select(start: anchor, length: 0)
        default:
            break
        }
    }

    override func mouseDragged(with event: NSEvent) {
        guard selecting, let layout = layout else { return }
        let p = mapPoint(event)
        defer { SelectionBadge.shared.show(for: document, event: event, in: self) }
        let n = document.length
        if oc_map_layout_is_circular(layout) {
            // Accumulate the swept angle so a drag can pass the origin.
            let a = oc_map_layout_angle_at(layout, p.x, p.y)
            var d = a - lastAngle
            if d > .pi { d -= 2 * .pi } else if d < -.pi { d += 2 * .pi }
            accumulated = max(-2 * .pi, min(2 * .pi, accumulated + d))
            lastAngle = a
            let len = Int((accumulated / (2 * .pi) * Double(n)).rounded())
            if len >= 0 {
                document.select(start: anchor, length: len)
            } else {
                document.select(start: anchor + len, length: -len)
            }
        } else {
            let pos = Int(oc_map_layout_position_at(layout, p.x, p.y))
            document.select(start: min(pos, anchor), length: abs(pos - anchor))
        }
    }

    override func mouseUp(with event: NSEvent) {
        if selecting { SelectionBadge.shared.hideSoon() }
        selecting = false
    }

    // MARK: Tooltips

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds,
                                       options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func mouseMoved(with event: NSEvent) {
        guard let layout = layout else { return }
        let p = mapPoint(event)
        let tip = takeString(oc_map_layout_tooltip(layout, p.x, p.y))
        toolTip = tip.isEmpty ? nil : tip
        // On the backbone: the base there, for the status.
        var kind: Int32 = 0, index: Int32 = 0, position: Int32 = 0
        oc_map_layout_hit(layout, p.x, p.y, &kind, &index, &position)
        if kind == Int32(OC_HIT_SEQUENCE) { onHoverBase?(Int(position) % max(1, document.length)) }
    }

    override func mouseExited(with event: NSEvent) {
        onHoverBase?(-1)
    }
}
