// SPDX-License-Identifier: GPL-3.0-or-later
// Draws a shared-layout scene with CoreGraphics. Expects a flipped context
// (y grows downward), as in a flipped NSView.
import AppKit
import OpenCloneCore

func drawScene(_ scene: OpaquePointer, in ctx: CGContext) {
    let background = oc_scene_background(scene)
    if background.a > 0 {
        ctx.setFillColor(background.cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: oc_scene_width(scene), height: oc_scene_height(scene)))
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

/// A map layout that follows the document's Map Display options.
func newMapLayout(_ document: CoreDocument, size: CGSize, dark: Bool) -> OpaquePointer? {
    oc_map_layout_new_options(document.handle, size.width, size.height, dark, document.mapShowsEnzymes,
                              document.mapShowsPrimers, document.mapHiddenTypes.sorted().joined(separator: ","),
                              measureText, nil)
}

/// Draws a scene into a bitmap image now, so the scene can be freed.
func sceneImage(_ scene: OpaquePointer) -> NSImage {
    let size = NSSize(width: oc_scene_width(scene), height: oc_scene_height(scene))
    let image = NSImage(size: size)
    image.lockFocusFlipped(true)
    if let ctx = NSGraphicsContext.current?.cgContext { drawScene(scene, in: ctx) }
    image.unlockFocus()
    return image
}

/// Writes a map to .png (2x) or .pdf, chosen by the file extension.
func writeMapImage(document: CoreDocument, to url: URL, size: CGSize) throws {
    guard let layout = newMapLayout(document, size: size, dark: false) else {
        throw NSError(domain: "OpenClone", code: 3, userInfo: [NSLocalizedDescriptionKey: "Could not lay out the map"])
    }
    defer { oc_map_layout_free(layout) }
    guard let scene = oc_map_layout_render(layout, document.handle) else {
        throw NSError(domain: "OpenClone", code: 3, userInfo: [NSLocalizedDescriptionKey: "Could not draw the map"])
    }
    defer { oc_scene_free(scene) }
    if url.pathExtension.lowercased() == "pdf" {
        var box = CGRect(origin: .zero, size: size)
        guard let pdf = CGContext(url as CFURL, mediaBox: &box, nil) else {
            throw NSError(domain: "OpenClone", code: 4, userInfo: [NSLocalizedDescriptionKey: "Could not create the PDF"])
        }
        pdf.beginPDFPage(nil)
        pdf.translateBy(x: 0, y: size.height)
        pdf.scaleBy(x: 1, y: -1)
        drawScene(scene, in: pdf)
        pdf.endPDFPage()
        pdf.closePDF()
        return
    }
    let scale = 2.0
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * scale),
                                     pixelsHigh: Int(size.height * scale), bitsPerSample: 8, samplesPerPixel: 4,
                                     hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                     bytesPerRow: 0, bitsPerPixel: 0),
          let gc = NSGraphicsContext(bitmapImageRep: rep) else {
        throw NSError(domain: "OpenClone", code: 5, userInfo: [NSLocalizedDescriptionKey: "Could not create the image"])
    }
    let cg = gc.cgContext
    cg.translateBy(x: 0, y: size.height * scale)
    cg.scaleBy(x: scale, y: -scale)
    drawScene(scene, in: cg)
    guard let png = rep.representation(using: .png, properties: [:]) else {
        throw NSError(domain: "OpenClone", code: 6, userInfo: [NSLocalizedDescriptionKey: "Could not encode the PNG"])
    }
    try png.write(to: url)
}
