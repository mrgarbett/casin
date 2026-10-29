// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import RestrictoCore

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

    // The sidebar's Not Saved section and red dots follow these.
    private var wasEdited = false

    override func updateChangeCount(_ change: NSDocument.ChangeType) {
        super.updateChangeCount(change)
        noteEditedState()
    }

    /// Saving clears the edited state through this one.
    override func updateChangeCount(withToken changeCountToken: Any, for saveOperation: NSDocument.SaveOperationType) {
        super.updateChangeCount(withToken: changeCountToken, for: saveOperation)
        noteEditedState()
    }

    private func noteEditedState() {
        if isDocumentEdited != wasEdited {
            wasEdited = isDocumentEdited
            Self.postDocumentsChanged()
        }
    }

    override func close() {
        super.close()
        Self.postDocumentsChanged()
    }

    /// After the current event, so the document list and edited flags have settled.
    static func postDocumentsChanged() {
        DispatchQueue.main.async {
            NotificationCenter.default.post(name: FolderBrowserController.documentsChanged, object: nil)
        }
    }

    override func read(from url: URL, ofType typeName: String) throws {
        try core.load(path: url.path)
    }

    override func makeWindowControllers() {
        addWindowController(DocumentWindowController(document: self))
        Self.postDocumentsChanged()
    }

    /// The type a file was opened as may be another app's (SnapGene's own
    /// .dna type, say), which the save panel doesn't list, so it would
    /// default to GenBank. Name ours instead, so Save As keeps the format.
    override var fileType: String? {
        get {
            switch fileURL?.pathExtension.lowercased() {
            case "dna": return "io.github.mrgarbett.restricto.snapgene-dna"
            case "embl", "emb": return "io.github.mrgarbett.restricto.embl"
            default: return super.fileType
            }
        }
        set { super.fileType = newValue }
    }

    /// The file's own format first: the save panel's format popup starts on
    /// the first one when the type the file was opened as isn't listed.
    override func writableTypes(for saveOperation: NSDocument.SaveOperationType) -> [String] {
        let types = ["io.github.mrgarbett.restricto.genbank", "io.github.mrgarbett.restricto.snapgene-dna",
                     "io.github.mrgarbett.restricto.embl"]
        guard let own = fileType, let i = types.firstIndex(of: own) else { return types }
        return [types[i]] + types.enumerated().filter { $0.offset != i }.map { $0.element }
    }

    override func write(to url: URL, ofType typeName: String) throws {
        let format: Int32
        switch typeName {
        case "io.github.mrgarbett.restricto.snapgene-dna": format = Int32(OC_FORMAT_SNAPGENE)
        case "io.github.mrgarbett.restricto.embl": format = Int32(OC_FORMAT_EMBL)
        case "io.github.mrgarbett.restricto.fasta": format = Int32(OC_FORMAT_FASTA)
        default: format = Int32(OC_FORMAT_GENBANK)
        }
        try core.save(path: url.path, format: format)
    }
}
