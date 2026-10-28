// SPDX-License-Identifier: GPL-3.0-or-later
// Map layouts and images for the window's document (the drawing itself is in
// RestrictoDrawing).
import AppKit
import RestrictoCore

/// A map layout that follows the document's Map Display options, with the
/// translation's ORFs as arrows while it is on.
func newMapLayout(_ document: CoreDocument, size: CGSize, dark: Bool) -> OpaquePointer? {
    oc_map_layout_new_frames(document.handle, size.width, size.height, dark, document.mapShowsEnzymes,
                             document.mapShowsPrimers, document.mapHiddenTypes.sorted().joined(separator: ","),
                             Int32(document.frames), measureText, nil)
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
    try mapImageData(document: document, size: size, pdf: url.pathExtension.lowercased() == "pdf").write(to: url)
}

/// Puts the map on the clipboard as a PDF (sharp when pasted into slides or
/// documents) and a 2x PNG, for apps that take only bitmaps.
func copyMapImageToPasteboard(document: CoreDocument, size: CGSize) throws {
    let pdf = try mapImageData(document: document, size: size, pdf: true)
    let png = try mapImageData(document: document, size: size, pdf: false)
    let board = NSPasteboard.general
    board.clearContents()
    board.declareTypes([.pdf, .png], owner: nil)
    board.setData(pdf, forType: .pdf)
    board.setData(png, forType: .png)
}

/// The map, light, as PDF or as a 2x PNG.
func mapImageData(document: CoreDocument, size: CGSize, pdf: Bool) throws -> Data {
    guard let layout = newMapLayout(document, size: size, dark: false) else {
        throw NSError(domain: "Restricto", code: 3, userInfo: [NSLocalizedDescriptionKey: "Could not lay out the map"])
    }
    defer { oc_map_layout_free(layout) }
    guard let scene = oc_map_layout_render(layout, document.handle) else {
        throw NSError(domain: "Restricto", code: 3, userInfo: [NSLocalizedDescriptionKey: "Could not draw the map"])
    }
    defer { oc_scene_free(scene) }
    if pdf {
        let data = NSMutableData()
        var box = CGRect(origin: .zero, size: size)
        guard let consumer = CGDataConsumer(data: data as CFMutableData),
              let ctx = CGContext(consumer: consumer, mediaBox: &box, nil) else {
            throw NSError(domain: "Restricto", code: 4, userInfo: [NSLocalizedDescriptionKey: "Could not create the PDF"])
        }
        ctx.beginPDFPage(nil)
        ctx.translateBy(x: 0, y: size.height)
        ctx.scaleBy(x: 1, y: -1)
        drawScene(scene, in: ctx)
        ctx.endPDFPage()
        ctx.closePDF()
        return data as Data
    }
    let scale = 2.0
    guard let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(size.width * scale),
                                     pixelsHigh: Int(size.height * scale), bitsPerSample: 8, samplesPerPixel: 4,
                                     hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
                                     bytesPerRow: 0, bitsPerPixel: 0),
          let gc = NSGraphicsContext(bitmapImageRep: rep) else {
        throw NSError(domain: "Restricto", code: 5, userInfo: [NSLocalizedDescriptionKey: "Could not create the image"])
    }
    let cg = gc.cgContext
    cg.translateBy(x: 0, y: size.height * scale)
    cg.scaleBy(x: scale, y: -scale)
    drawScene(scene, in: cg)
    guard let png = rep.representation(using: .png, properties: [:]) else {
        throw NSError(domain: "Restricto", code: 6, userInfo: [NSLocalizedDescriptionKey: "Could not encode the PNG"])
    }
    return png
}
