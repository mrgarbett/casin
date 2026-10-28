// SPDX-License-Identifier: GPL-3.0-or-later
// Draws shared-layout scenes with CoreGraphics, for the app and its Quick Look
// extension. Scenes expect a flipped context (y grows downward), as in a
// flipped NSView.
import AppKit
import RestrictoCore

/// Takes ownership of a malloc'd C string from the core.
public func takeString(_ p: UnsafeMutablePointer<CChar>?) -> String {
    guard let p = p else { return "" }
    defer { oc_string_free(p) }
    return String(cString: p)
}

extension OCColor {
    public var nsColor: NSColor { NSColor(srgbRed: r, green: g, blue: b, alpha: a) }
    public var cgColor: CGColor { CGColor(srgbRed: r, green: g, blue: b, alpha: a) }
}

// MARK: - Fonts and text metrics

public enum SceneFonts {
    public static func font(size: Double, bold: Bool, mono: Bool) -> NSFont {
        if mono {
            return NSFont.monospacedSystemFont(ofSize: size, weight: bold ? .bold : .regular)
        }
        return NSFont.systemFont(ofSize: size, weight: bold ? .semibold : .regular)
    }
}

/// Text widths for the shared layout code, measured with AppKit fonts.
public let measureText: OCMeasureText = { _, text, size, bold, mono in
    guard let text = text else { return 0 }
    let font = SceneFonts.font(size: size, bold: bold, mono: mono)
    return Double((String(cString: text) as NSString).size(withAttributes: [.font: font]).width)
}

/// `trailing` widens the background fill that far right of the scene, for a
/// view that keeps an inset free on its right.
public func drawScene(_ scene: OpaquePointer, in ctx: CGContext, trailing: CGFloat = 0) {
    let background = oc_scene_background(scene)
    if background.a > 0 {
        ctx.setFillColor(background.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: oc_scene_width(scene) + trailing, height: oc_scene_height(scene)))
    }
    ctx.setLineJoin(.round)
    let count = Int(oc_scene_item_count(scene))
    for i in 0..<count {
        let index = Int32(i)
        if oc_scene_item_kind(scene, index) == Int32(OC_ITEM_PATH) {
            var p = OCPathInfo()
            guard oc_scene_path(scene, index, &p), p.point_count >= 2, let pts = p.points else { continue }
            let path = CGMutablePath()
            path.move(to: CGPoint(x: pts[0].x, y: pts[0].y))
            for j in 1..<Int(p.point_count) {
                path.addLine(to: CGPoint(x: pts[j].x, y: pts[j].y))
            }
            if p.closed { path.closeSubpath() }
            if p.fill.a > 0 {
                ctx.addPath(path)
                ctx.setFillColor(p.fill.cgColor)
                ctx.fillPath()
            }
            if p.stroke.a > 0 {
                ctx.addPath(path)
                ctx.setStrokeColor(p.stroke.cgColor)
                ctx.setLineWidth(p.line_width)
                ctx.setLineDash(phase: 0, lengths: p.dashed ? [3, 3] : [])
                ctx.strokePath()
            }
        } else {
            var t = OCTextInfo()
            guard oc_scene_text(scene, index, &t), let cText = t.text else { continue }
            drawText(String(cString: cText), t, in: ctx)
        }
    }
}

private func drawText(_ string: String, _ t: OCTextInfo, in ctx: CGContext) {
    let font = SceneFonts.font(size: t.size, bold: t.bold, mono: t.mono)
    let attributed = NSAttributedString(string: string, attributes: [
        .font: font,
        .foregroundColor: t.color.nsColor,
    ])
    let size = attributed.size()
    var dx = 0.0
    switch t.halign {
    case Int32(OC_HALIGN_CENTER): dx = -size.width / 2
    case Int32(OC_HALIGN_RIGHT): dx = -size.width
    default: break
    }
    var dy = 0.0   // offset to the top of the line box
    switch t.valign {
    case Int32(OC_VALIGN_MIDDLE): dy = -size.height / 2
    case Int32(OC_VALIGN_TOP): dy = 0
    default: dy = -font.ascender
    }
    ctx.saveGState()
    ctx.translateBy(x: t.x, y: t.y)
    if t.angle != 0 {
        ctx.rotate(by: t.angle)   // clockwise on screen, since the context is flipped
    }
    // NSAttributedString draws through the current NSGraphicsContext.
    let previous = NSGraphicsContext.current
    NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: true)
    attributed.draw(at: NSPoint(x: dx, y: dy))
    NSGraphicsContext.current = previous
    ctx.restoreGState()
}

/// Draws the circular (or linear) map of `document` (an OCDocument) into a
/// flipped context of the given size, light, with no ORFs.
@discardableResult
public func drawMap(of document: OpaquePointer, size: CGSize, in ctx: CGContext) -> Bool {
    guard let layout = oc_map_layout_new_frames(document, size.width, size.height, false, true, true, nil, 0,
                                                measureText, nil) else { return false }
    defer { oc_map_layout_free(layout) }
    guard let scene = oc_map_layout_render(layout, document) else { return false }
    defer { oc_scene_free(scene) }
    drawScene(scene, in: ctx)
    return true
}
