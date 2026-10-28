// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import OpenCloneCore

/// One open sequence. Saves as GenBank, SnapGene .dna or EMBL (chosen in the
/// save panel); FASTA stays an export. Edits, undo and redo live in the shared
/// core; the window's edited dot follows the core's modified flag.
@objc(SequenceDocument)   // the name Info.plist's NSDocumentClass refers to
final class SequenceDocument: NSDocument {
    let core: CoreDocument

    override init() {
        core = CoreDocument()
        super.init()
        followModifiedFlag()
    }

    /// A document around a product the core has already filled in (an
    /// assembly, a PCR product, typed bases): unsaved and untitled.
    init(core: CoreDocument) {
        self.core = core
        super.init()
        followModifiedFlag()
    }

    /// Opens `core` in a new window as an unsaved document.
    static func open(_ core: CoreDocument) {
        let doc = SequenceDocument(core: core)
        NSDocumentController.shared.addDocument(doc)
        doc.makeWindowControllers()
        doc.showWindows()
        doc.updateChangeCount(.changeDone)
    }

    private func followModifiedFlag() {
        core.observe { [weak self] what in
            guard let self = self, what & Change.sequence != 0 else { return }
            self.updateChangeCount(self.core.isModified ? .changeDone : .changeCleared)
        }
    }

    override class var autosavesInPlace: Bool { false }

    override func read(from url: URL, ofType typeName: String) throws {
        try core.load(path: url.path)
    }

    override func makeWindowControllers() {
        addWindowController(DocumentWindowController(document: self))
    }

    override func writableTypes(for saveOperation: NSDocument.SaveOperationType) -> [String] {
        ["io.github.openclone.genbank", "io.github.openclone.snapgene-dna", "io.github.openclone.embl"]
    }

    override func write(to url: URL, ofType typeName: String) throws {
        let format: Int32
        switch typeName {
        case "io.github.openclone.snapgene-dna": format = Int32(OC_FORMAT_SNAPGENE)
        case "io.github.openclone.embl": format = Int32(OC_FORMAT_EMBL)
        case "io.github.openclone.fasta": format = Int32(OC_FORMAT_FASTA)
        default: format = Int32(OC_FORMAT_GENBANK)
        }
        try core.save(path: url.path, format: format)
    }
}
