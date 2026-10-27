// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import OpenCloneCore

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

    init(document: CoreDocument) {
        self.document = document
        super.init(frame: NSRect(x: 0, y: 0, width: 520, height: 520))
        document.observe { [weak self] what in
            guard let self = self else { return }
            if what & (Change.sequence | Change.enzymes) != 0 { self.invalidateLayout() }
            self.needsDisplay = true
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    deinit {
        if let layout = layout { oc_map_layout_free(layout) }
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
        if layout == nil || layoutSize != bounds.size || layoutDark != dark {
            if let old = layout { oc_map_layout_free(old) }
            layout = newMapLayout(document, size: bounds.size, dark: dark)
            layoutSize = bounds.size
            layoutDark = dark
        }
        return layout
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext, let layout = ensureLayout(),
              let scene = oc_map_layout_render(layout, document.handle) else { return }
        defer { oc_scene_free(scene) }
        drawScene(scene, in: ctx)
    }

    // MARK: Mouse

    override func mouseDown(with event: NSEvent) {
        guard let layout = ensureLayout(), !document.isEmpty else { return }
        let p = convert(event.locationInWindow, from: nil)
        var kind: Int32 = 0, index: Int32 = 0, position: Int32 = 0
        oc_map_layout_hit(layout, p.x, p.y, &kind, &index, &position)
        selecting = false
        switch kind {
        case Int32(OC_HIT_FEATURE):
            document.activateFeature(Int(index))
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
        let p = convert(event.locationInWindow, from: nil)
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
        selecting = false
    }

    // MARK: Tooltips

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func mouseMoved(with event: NSEvent) {
        guard let layout = layout else { return }
        let p = convert(event.locationInWindow, from: nil)
        let tip = takeString(oc_map_layout_tooltip(layout, p.x, p.y))
        toolTip = tip.isEmpty ? nil : tip
    }
}
