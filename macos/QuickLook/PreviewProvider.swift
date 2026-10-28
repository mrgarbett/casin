// SPDX-License-Identifier: GPL-3.0-or-later
// The Quick Look preview extension: pressing Space on a sequence file in
// Finder shows its map, drawn by the same core as the app.
import AppKit
import QuickLookUI
import RestrictoCore
import RestrictoDrawing

@objc(PreviewProvider)
final class PreviewProvider: QLPreviewProvider, QLPreviewingController {
    func providePreview(for request: QLFilePreviewRequest,
                        completionHandler handler: @escaping (QLPreviewReply?, Error?) -> Void) {
        let document = oc_document_new()
        var err: UnsafeMutablePointer<CChar>?
        guard let document = document, oc_document_load(document, request.fileURL.path, &err) else {
            if let document = document { oc_document_free(document) }
            handler(nil, NSError(domain: "Restricto", code: 1,
                                 userInfo: [NSLocalizedDescriptionKey: takeString(err)]))
            return
        }
        let size = CGSize(width: 820, height: 700)
        let reply = QLPreviewReply(contextSize: size, isBitmap: true) { ctx, _ in
            defer { oc_document_free(document) }
            // The reply's context has y growing upward; scenes want it flipped.
            ctx.translateBy(x: 0, y: size.height)
            ctx.scaleBy(x: 1, y: -1)
            drawMap(of: document, size: size, in: ctx)
        }
        handler(reply, nil)
    }
}
