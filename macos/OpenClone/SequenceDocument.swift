// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit

/// One open sequence file. OpenClone is a viewer in this milestone: files are
/// never written back; use Save As GenBank or Export FASTA instead.
@objc(SequenceDocument)   // the name Info.plist's NSDocumentClass refers to
final class SequenceDocument: NSDocument {
    let core = CoreDocument()

    override class var autosavesInPlace: Bool { false }

    override func read(from url: URL, ofType typeName: String) throws {
        try core.load(path: url.path)
    }

    override func makeWindowControllers() {
        addWindowController(DocumentWindowController(document: self))
    }

    override func data(ofType typeName: String) throws -> Data {
        throw NSError(domain: NSCocoaErrorDomain, code: NSFeatureUnsupportedError,
                      userInfo: [NSLocalizedDescriptionKey: "Use File > Save As GenBank or Export FASTA."])
    }
}
