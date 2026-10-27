// SPDX-License-Identifier: GPL-3.0-or-later
// The editing, primer, cloning and verification tools, as menus over the C API.
// Each action works on the front window's document and opens products in new
// windows, as the Linux app does. Plain AppKit: the same on every macOS version.
import AppKit
import OpenCloneCore
import UniformTypeIdentifiers

enum ToolMenus {
    private static func item(_ title: String, _ action: Selector, _ key: String = "",
                             _ modifiers: NSEvent.ModifierFlags = .command) -> NSMenuItem {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
        i.keyEquivalentModifierMask = modifiers
        return i
    }

    private static func menu(_ title: String, _ items: [NSMenuItem]) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let menu = NSMenu(title: title)
        items.forEach(menu.addItem)
        item.submenu = menu
        return item
    }

    private typealias W = DocumentWindowController

    /// Adds the tools to the main menu: editing and saving to File and Edit,
    /// display options to View, and Sequence, Features, Primers, Cloning and
    /// Align menus before View.
    static func install(in main: NSMenu) {
        if let file = main.item(withTitle: "File")?.submenu {
            // Real saving replaces the old "Save As GenBank" export.
            if let old = file.item(withTitle: "Save As GenBank…") { file.removeItem(old) }
            let openIndex = file.indexOfItem(withTitle: "Open…")
            file.insertItem(item("New from Bases…", #selector(W.newFromBases(_:)), "n"), at: max(0, openIndex))
            let closeIndex = file.indexOfItem(withTitle: "Close")
            var at = closeIndex >= 0 ? closeIndex + 1 : file.numberOfItems
            for i in [item("Save", #selector(NSDocument.save(_:)), "s"),
                      item("Save As…", #selector(NSDocument.saveAs(_:)), "s", [.command, .shift]),
                      item("Revert to Saved", #selector(NSDocument.revertToSaved(_:)))] {
                file.insertItem(i, at: at)
                at += 1
            }
            file.addItem(.separator())
            file.addItem(item("Search a Folder…", #selector(W.searchFolder(_:)), "f", [.command, .shift]))
            file.addItem(.separator())
            file.addItem(item("Print…", #selector(W.printSequence(_:)), "p"))
        }
        if let edit = main.item(withTitle: "Edit")?.submenu {
            let top = [item("Undo", #selector(W.undoEdit(_:)), "z"),
                       item("Redo", #selector(W.redoEdit(_:)), "z", [.command, .shift]),
                       .separator(),
                       item("Cut", #selector(SequenceView.cut(_:)), "x"),
                       item("Paste", #selector(SequenceView.paste(_:)), "v"),
                       item("Delete", #selector(SequenceView.delete(_:))),
                       .separator()]
            for (i, it) in top.enumerated() { edit.insertItem(it, at: i) }
        }
        let tools = [
            menu("Sequence", [
                item("Reverse Complement", #selector(W.reverseComplementSequence(_:))),
                item("Set Origin at Selection", #selector(W.setOrigin(_:))),
                item("Circular", #selector(W.toggleCircular(_:))),
                item("Rename…", #selector(W.renameSequence(_:))),
                .separator(),
                item("Choose Enzymes…", #selector(W.chooseEnzymes(_:))),
                item("Enzyme Set…", #selector(W.enzymeSet(_:))),
                item("Digest and Gel…", #selector(W.digestAndGel(_:)), "g", [.command, .shift]),
            ]),
            menu("Features", [
                item("Add Feature…", #selector(W.addFeature(_:))),
                item("Delete Feature", #selector(W.deleteFeature(_:))),
                .separator(),
                item("Find ORFs…", #selector(W.findOrfs(_:))),
                item("Protein Properties…", #selector(W.proteinProperties(_:))),
            ]),
            menu("Primers", [
                item("Add Primer from Selection…", #selector(W.addPrimerFromSelection(_:))),
                item("Add Primer by Sequence…", #selector(W.addPrimerBySequence(_:))),
                item("Delete Primer…", #selector(W.deletePrimer(_:))),
                .separator(),
                item("Design Primers…", #selector(W.designPrimers(_:))),
                item("Check a Primer…", #selector(W.checkPrimer(_:))),
            ]),
            menu("Cloning", [
                item("PCR Product…", #selector(W.pcrProduct(_:))),
                item("Restriction Cloning…", #selector(W.restrictionCloning(_:))),
                item("Golden Gate Assembly…", #selector(W.goldenGate(_:))),
                item("Gibson Assembly…", #selector(W.gibson(_:))),
                item("TA, GC and TOPO Cloning…", #selector(W.overhangCloning(_:))),
                item("Overlap Extension PCR…", #selector(W.overlapExtension(_:))),
                item("Insertion by PCR…", #selector(W.insertionByPcr(_:))),
                item("Mutagenesis…", #selector(W.mutagenesis(_:))),
                .separator(),
                item("Cloning History", #selector(W.cloningHistory(_:))),
            ]),
            menu("Align", [
                item("Align Read…", #selector(W.alignRead(_:))),
                item("Show All Reads", #selector(W.showAllReads(_:))),
                item("Compare with Another Sequence…", #selector(W.compareSequences(_:))),
                .separator(),
                item("Hide Aligned Reads", #selector(W.hideReads(_:))),
            ]),
        ]
        let viewIndex = main.indexOfItem(withTitle: "View")
        var at = viewIndex >= 0 ? viewIndex : main.numberOfItems
        for t in tools {
            main.insertItem(t, at: at)
            at += 1
        }
        if let view = main.item(withTitle: "View")?.submenu {
            let frames = NSMenu(title: "Translate")
            frames.addItem(item("No Translation", #selector(W.setFrames(_:))))
            frames.addItem(item("Three Forward Frames", #selector(W.setFrames(_:))))
            frames.addItem(item("All Six Frames", #selector(W.setFrames(_:))))
            for (i, f) in frames.items.enumerated() { f.tag = [0, 3, 6][i] }
            let translate = NSMenuItem(title: "Translate", action: nil, keyEquivalent: "")
            translate.submenu = frames
            view.insertItem(translate, at: 0)
            view.insertItem(item("Map Display…", #selector(W.mapDisplay(_:))), at: 1)
            view.insertItem(.separator(), at: 2)
        }
    }
}

// MARK: - Actions

extension DocumentWindowController {
    /// The document's core; the tools act on it.
    private var doc: CoreDocument? { (document as? SequenceDocument)?.core }

    /// Other open sequences, for inserts, fragments and reads.
    private func others(linearOnly: Bool = false) -> [CoreDocument] {
        NSDocumentController.shared.documents.compactMap { ($0 as? SequenceDocument)?.core }.filter {
            $0 !== doc && !$0.isEmpty && (!linearOnly || !$0.isCircular)
        }
    }

    private func names(_ docs: [CoreDocument]) -> [String] {
        docs.map { "\($0.name.isEmpty ? "Untitled" : $0.name) (\($0.length.formatted()) bp)" }
    }

    private func tell(_ title: String, _ message: String) {
        showText(title, message, "", in: window)
    }

    /// Runs a core call that fills a new document, then opens it.
    private func product(_ title: String, _ make: (OpaquePointer, UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>) -> Bool) {
        let out = CoreDocument()
        var err: UnsafeMutablePointer<CChar>?
        if make(out.handle, &err) {
            SequenceDocument.open(out)
        } else {
            showError(title, err, in: window)
        }
    }

    private var hasRange: Bool {
        guard let d = doc else { return false }
        return d.hasSelection && d.selectionLength > 0
    }

    // MARK: File

    @objc func newFromBases(_ sender: Any?) {
        let form = Form("New Sequence", "Type or paste bases; anything that isn't DNA is left out.")
        let name = form.text("Name", "Untitled")
        let bases = form.text("Bases", "", placeholder: "ACGT…", width: 320)
        let circular = form.check("", "Circular", on: false)
        form.run(in: window, ok: "Create") {
            let core = CoreDocument()
            if oc_document_new_sequence(core.handle, name.stringValue, bases.stringValue, circular.state == .on) {
                SequenceDocument.open(core)
            } else {
                showText("New Sequence", "There were no DNA bases to use.", "", in: nil)
            }
        }
    }

    @objc func searchFolder(_ sender: Any?) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.message = "Choose a folder to search"
        let run: (URL) -> Void = { [weak self] folder in
            let form = Form("Search \(folder.lastPathComponent)",
                            "Names, descriptions, features and primers, or DNA on both strands (4 bases or more).")
            let query = form.text("Find")
            form.run(in: self?.window, ok: "Search") {
                let q = query.stringValue.trimmingCharacters(in: .whitespaces)
                guard !q.isEmpty else { return }
                let lines = takeString(oc_search_folder(folder.path, q)).split(separator: "\n")
                if lines.isEmpty {
                    showText("No Matches", "Nothing under \(folder.lastPathComponent) matches \(q).", "", in: self?.window)
                    return
                }
                let hits = lines.map { $0.split(separator: "\t", omittingEmptySubsequences: false).map(String.init) }
                let pick = Form("\(hits.count) Match\(hits.count == 1 ? "" : "es")", "Open one:")
                let list = pick.popup("File", hits.map { (h: [String]) -> String in
                    h.count >= 5 ? "\(h[1]) · \(Int(h[2])?.formatted() ?? h[2]) bp · \(h[4])" : h.joined(separator: " ")
                })
                pick.run(in: self?.window, ok: "Open") {
                    let h = hits[list.indexOfSelectedItem]
                    NSDocumentController.shared.openDocument(withContentsOf: URL(fileURLWithPath: h[0]),
                                                             display: true) { _, _, error in
                        if let error = error { NSApp.presentError(error) }
                    }
                }
            }
        }
        if let window = window {
            panel.beginSheetModal(for: window) { r in
                if r == .OK, let url = panel.url { DispatchQueue.main.async { run(url) } }
            }
        } else if panel.runModal() == .OK, let url = panel.url {
            run(url)
        }
    }

    @objc func printSequence(_ sender: Any?) {
        guard let d = doc, !d.isEmpty, let window = window else { return }
        let info = (NSPrintInfo.shared.copy() as? NSPrintInfo) ?? NSPrintInfo.shared
        info.horizontalPagination = .fit
        info.verticalPagination = .automatic
        let page = info.imageablePageBounds.size
        let view = PrintView(document: d, page: page)
        let op = NSPrintOperation(view: view, printInfo: info)
        op.jobTitle = d.name
        op.runModal(for: window, delegate: nil, didRun: nil, contextInfo: nil)
    }

    // MARK: Edit

    @objc func undoEdit(_ sender: Any?) {
        guard let d = doc, oc_document_can_undo(d.handle) else { NSSound.beep(); return }
        oc_document_undo(d.handle)
    }

    @objc func redoEdit(_ sender: Any?) {
        guard let d = doc, oc_document_can_redo(d.handle) else { NSSound.beep(); return }
        oc_document_redo(d.handle)
    }

    // MARK: Sequence

    @objc func reverseComplementSequence(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        oc_document_reverse_complement(d.handle)
    }

    @objc func setOrigin(_ sender: Any?) {
        guard let d = doc, d.isCircular, d.hasSelection else {
            tell("Set Origin", "Place the caret in a circular sequence where base 1 should be.")
            return
        }
        oc_document_set_origin_at_selection(d.handle)
    }

    @objc func toggleCircular(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        oc_document_set_circular(d.handle, !d.isCircular)
    }

    @objc func renameSequence(_ sender: Any?) {
        guard let d = doc else { return }
        let form = Form("Rename Sequence")
        let name = form.text("Name", d.name)
        form.run(in: window, ok: "Rename") {
            let n = name.stringValue.trimmingCharacters(in: .whitespaces)
            if !n.isEmpty { oc_document_rename(d.handle, n) }
        }
    }

    @objc func chooseEnzymes(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let form = Form("Choose Enzymes", "Find enzymes for cloning and show only those on the map and sequence.")
        let cuts = form.popup("Cuts", ["Once", "Up to twice", "Up to three times"])
        let inside = form.check("", "Only inside the selection", on: hasRange)
        let features = d.features.filter { $0.drawn }
        let spare = form.popup("Never inside", ["(no feature)"] + features.map { $0.name })
        let ends = form.popup("Ends", ["Any", "5′ overhang", "3′ overhang", "Blunt"])
        form.run(in: window, ok: "Show") { [weak self] in
            let spareIndex = spare.indexOfSelectedItem > 0 ? features[spare.indexOfSelectedItem - 1].index : -1
            let maxCuts = cuts.indexOfSelectedItem + 1
            let list = takeString(oc_document_choose_enzymes(d.handle, Int32(maxCuts), inside.state == .on,
                                                              Int32(spareIndex), Int32(ends.indexOfSelectedItem)))
            let found = list.split(separator: "\n").map(String.init)
            if found.isEmpty {
                self?.tell("Choose Enzymes", "No enzyme fits all of that.")
                return
            }
            withCStrings(found) { oc_document_show_only_enzymes(d.handle, $0, Int32(found.count), Int32(maxCuts)) }
            self?.tell("Choose Enzymes", "Showing \(found.count): \(found.joined(separator: ", ")).")
        }
    }

    @objc func enzymeSet(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let sets: [(String, [String])] = takeString(oc_builtin_enzyme_sets()).split(separator: "\n").compactMap {
            line -> (String, [String])? in
            let parts = line.split(separator: "\t", maxSplits: 1).map(String.init)
            guard parts.count == 2 else { return nil }
            return (parts[0], parts[1].split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) })
        }
        let form = Form("Enzyme Set", "Show only the enzymes in a set. The cut filter still applies.")
        let pick = form.popup("Set", sets.map { "\($0.0) (\($0.1.count))" })
        form.run(in: window, ok: "Show") {
            guard pick.indexOfSelectedItem >= 0 && pick.indexOfSelectedItem < sets.count else { return }
            let names = sets[pick.indexOfSelectedItem].1
            withCStrings(names) { oc_document_show_enzyme_set(d.handle, $0, Int32(names.count)) }
        }
    }

    @objc func digestAndGel(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let shown = Array(Set(d.visibleSiteEnzymes)).sorted()
        let form = Form("Digest and Gel", "Enzymes shown on the map, cut together.")
        let first = form.popup("Enzyme", shown)
        let second = form.popup("And", ["(none)"] + shown)
        let ladders = (0..<Int(oc_ladder_count())).map { String(cString: oc_ladder_name(Int32($0))) }
        let ladder = form.popup("Ladder", ladders)
        guard !shown.isEmpty else {
            tell("Digest and Gel", "No enzymes are shown. Change the cut filter in the Enzymes sidebar.")
            return
        }
        form.run(in: window, ok: "Digest") { [weak self] in
            var enzymes = [shown[max(0, first.indexOfSelectedItem)]]
            if second.indexOfSelectedItem > 0 { enzymes.append(shown[second.indexOfSelectedItem - 1]) }
            var frags = [OCFragmentInfo](repeating: OCFragmentInfo(), count: 400)
            let n = withCStrings(enzymes) { oc_digest(d.handle, $0, Int32(enzymes.count), &frags, Int32(frags.count)) }
            var lines: [String] = []
            for f in frags.prefix(Int(min(n, 400))) {
                let left = tupleString(f.left_enzyme), right = tupleString(f.right_enzyme)
                lines.append("\(Int(f.length).formatted()) bp".padding(toLength: 12, withPad: " ", startingAt: 0)
                             + "\(f.start + 1)  \(left.isEmpty ? "end" : left) – \(right.isEmpty ? "end" : right)")
            }
            let scene = withCStrings(enzymes) {
                oc_gel_render(d.handle, $0, Int32(enzymes.count), Int32(max(0, ladder.indexOfSelectedItem)), 360, 420,
                              self?.window?.contentView?.isDarkAppearance ?? false, measureText, nil)
            }
            let title = "\(enzymes.joined(separator: " + ")): \(n) fragment\(n == 1 ? "" : "s")"
            if let scene = scene {
                let image = sceneImage(scene)
                oc_scene_free(scene)
                showImage(title, lines.joined(separator: "\n"), image, in: self?.window)
            } else {
                showText(title, "", lines.joined(separator: "\n"), in: self?.window)
            }
        }
    }

    // MARK: Features

    @objc func addFeature(_ sender: Any?) {
        guard let d = doc, hasRange else { tell("Add Feature", "Select the bases the feature covers first."); return }
        let form = Form("Add Feature", "On \(d.selectionStart + 1)–\(d.selectionStart + d.selectionLength).")
        let name = form.text("Name", "feature")
        let types = ["misc_feature", "CDS", "gene", "promoter", "terminator", "RBS", "rep_origin", "primer_bind",
                     "protein_bind", "polyA_signal", "enhancer", "LTR", "sig_peptide", "mobile_element"]
        let type = form.popup("Type", types)
        let strand = form.popup("Strand", ["Forward", "Reverse", "None"])
        form.run(in: window, ok: "Add") {
            let strands = [OC_STRAND_FORWARD, OC_STRAND_REVERSE, OC_STRAND_NONE]
            let i = oc_document_add_feature(d.handle, name.stringValue, types[type.indexOfSelectedItem],
                                            Int32(strands[strand.indexOfSelectedItem]), Int32(d.selectionStart),
                                            Int32(d.selectionLength))
            if i >= 0 { d.activateFeature(Int(i)) }
        }
    }

    @objc func deleteFeature(_ sender: Any?) {
        guard let d = doc, d.activeFeature >= 0 else {
            tell("Delete Feature", "Click a feature on the map, the sequence or the sidebar first.")
            return
        }
        oc_document_remove_feature(d.handle, Int32(d.activeFeature))
    }

    @objc func findOrfs(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let form = Form("Find ORFs", "ATG to stop on both strands, longest first.")
        let minCodons = form.number("Minimum codons", 100)
        form.run(in: window, ok: "Find") { [weak self] in
            let codons = Int32(max(10, minCodons.number(or: 100)))
            let orfs = takeString(oc_find_orfs(d.handle, codons)).split(separator: "\n").map {
                $0.split(separator: "\t").map(String.init)
            }
            guard !orfs.isEmpty else {
                self?.tell("Find ORFs", "No ORFs of \(codons) codons or more.")
                return
            }
            let shown = Array(orfs.prefix(30))
            let pick = Form("\(orfs.count) ORF\(orfs.count == 1 ? "" : "s")", "Add the checked ones as CDS features.")
            let boxes = pick.checks("ORFs", shown.map { o -> String in
                guard o.count >= 4, let start = Int(o[0]), let len = Int(o[1]) else { return o.joined(separator: " ") }
                return "\(start + 1)–\(start + len) \(o[2] == "-1" ? "←" : "→") \((len / 3 - 1).formatted()) aa"
            })
            pick.run(in: self?.window, ok: "Add") {
                for (i, box) in boxes.enumerated() where box.state == .on {
                    oc_document_add_orf(d.handle, codons, Int32(i), "ORF \(i + 1)")
                }
            }
        }
    }

    @objc func proteinProperties(_ sender: Any?) {
        guard let d = doc else { return }
        var info = OCProteinInfo()
        guard let p = oc_document_selection_protein(d.handle, &info) else {
            tell("Protein Properties", "Select the coding bases, or a CDS feature, first.")
            return
        }
        let protein = takeString(p)
        var lines = [
            "Length              \(info.length) aa",
            "Molecular weight    \(String(format: "%.2f", info.mass / 1000)) kDa",
            "Isoelectric point   \(String(format: "%.2f", info.pI))",
            "Charged residues    \(info.positive) + (K, R), \(info.negative) − (D, E)",
            "Extinction, 280 nm  \(Int(info.extinction_reduced).formatted()) M⁻¹cm⁻¹ (Cys reduced)",
        ]
        if info.extinction_cystines != info.extinction_reduced {
            lines.append("  with cystines     \(Int(info.extinction_cystines).formatted()) M⁻¹cm⁻¹")
        }
        if info.truncated { lines.append("A stop codon ends the protein early; figures are up to it.") }
        showText("Protein Properties", lines.joined(separator: "\n"), protein, in: window)
    }

    // MARK: Primers

    @objc func addPrimerFromSelection(_ sender: Any?) {
        guard let d = doc, hasRange else { tell("Add Primer", "Select the bases the primer binds first."); return }
        let form = Form("Add Primer", "Binds \(d.selectionStart + 1)–\(d.selectionStart + d.selectionLength).")
        let name = form.text("Name", "primer")
        let strand = form.popup("Reads", ["Top strand (forward)", "Bottom strand (reverse)"])
        form.run(in: window, ok: "Add") {
            oc_document_add_primer_from_selection(d.handle, name.stringValue,
                                                  Int32(strand.indexOfSelectedItem == 0 ? OC_STRAND_FORWARD
                                                                                         : OC_STRAND_REVERSE))
        }
    }

    @objc func addPrimerBySequence(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let form = Form("Add Primer by Sequence", "Placed where its 3′ end binds best; a 5′ tail is fine.")
        let name = form.text("Name", "primer")
        let bases = form.text("Sequence", "", placeholder: "5′ → 3′", width: 320)
        form.run(in: window, ok: "Add") { [weak self] in
            var err: UnsafeMutablePointer<CChar>?
            if oc_document_add_primer_sequence(d.handle, name.stringValue, bases.stringValue, &err) < 0 {
                showError("Add Primer by Sequence", err, in: self?.window)
            }
        }
    }

    @objc func deletePrimer(_ sender: Any?) {
        guard let d = doc else { return }
        let primers = primerList(d)
        guard !primers.isEmpty else { tell("Delete Primer", "This sequence has no primers."); return }
        let form = Form("Delete Primer")
        let pick = form.popup("Primer", primers.map { $0.label })
        form.run(in: window, ok: "Delete") {
            oc_document_remove_primer(d.handle, Int32(pick.indexOfSelectedItem))
        }
    }

    private func primerList(_ d: CoreDocument) -> [(label: String, info: OCPrimerInfo)] {
        (0..<Int(oc_document_primer_count(d.handle))).compactMap { i in
            var p = OCPrimerInfo()
            guard oc_document_primer(d.handle, Int32(i), &p) else { return nil }
            let site = p.length > 0 ? "\(p.start + 1)–\(p.start + p.length) \(p.strand == Int32(OC_STRAND_REVERSE) ? "←" : "→")"
                                    : "doesn't bind"
            return ("\(String(cString: p.name)) · \(site)", p)
        }
    }

    @objc func designPrimers(_ sender: Any?) {
        guard let d = doc, hasRange else { tell("Design Primers", "Select the region to amplify first."); return }
        let form = Form("Design Primers", "A pair that amplifies \(d.selectionLength.formatted()) bp.")
        let tm = form.number("Target Tm (°C)", 60)
        let flank = form.number("Search outside by (bp)", d.selectionLength < 18 ? 100 : 0)
        form.run(in: window, ok: "Design") { [weak self] in
            var err: UnsafeMutablePointer<CChar>?
            if !oc_document_design_primers_flank(d.handle, tm.number(or: 60), Int32(max(0, flank.number(or: 0))), &err) {
                showError("Design Primers", err, in: self?.window)
            }
        }
    }

    @objc func checkPrimer(_ sender: Any?) {
        let form = Form("Check a Primer")
        let bases = form.text("Sequence", hasRange ? (doc?.selectedBases ?? "") : "", placeholder: "5′ → 3′", width: 320)
        form.run(in: window, ok: "Check") { [weak self] in
            var c = OCPrimerCheck()
            let warnings = takeString(oc_primer_check(bases.stringValue, &c))
            let lines = [
                "Length          \(c.length) nt",
                "GC              \(String(format: "%.0f", c.gc * 100))%",
                "Tm              \(c.tm.isNaN ? "—" : String(format: "%.1f °C", c.tm))",
                "3′ GC clamp     \(c.clamp_gc) of the last 5",
                "Longest run     \(c.longest_run)",
                "Self-dimer      \(c.self_dimer) bp (\(c.self_dimer3) at the 3′ end)",
                "Hairpin stem    \(c.hairpin_stem) bp",
            ]
            showText("Primer Check", warnings.isEmpty ? "Looks fine." : warnings, lines.joined(separator: "\n"),
                     in: self?.window)
        }
    }

    // MARK: Cloning

    @objc func pcrProduct(_ sender: Any?) {
        guard let d = doc else { return }
        let primers = primerList(d)
        guard primers.count >= 2 else {
            tell("PCR Product", "Add a forward and a reverse primer to this sequence first.")
            return
        }
        let form = Form("PCR Product")
        let fwd = form.popup("Forward", primers.map { $0.label },
                             selected: primers.firstIndex { $0.info.strand == Int32(OC_STRAND_FORWARD) } ?? 0)
        let rev = form.popup("Reverse", primers.map { $0.label },
                             selected: primers.firstIndex { $0.info.strand == Int32(OC_STRAND_REVERSE) } ?? 1)
        form.run(in: window, ok: "Open Product") { [weak self] in
            self?.product("PCR Product") {
                oc_pcr_product(d.handle, Int32(fwd.indexOfSelectedItem), Int32(rev.indexOfSelectedItem), $0, $1)
            }
        }
    }

    @objc func restrictionCloning(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let inserts = others()
        guard !inserts.isEmpty else { tell("Restriction Cloning", "Open the insert in another window first."); return }
        let all = (0..<Int(oc_document_enzyme_count(d.handle))).compactMap { i -> String? in
            var e = OCEnzymeInfo()
            return oc_document_enzyme(d.handle, Int32(i), &e) ? String(cString: e.name) : nil
        }
        let shown = Array(Set(d.visibleSiteEnzymes)).sorted()
        let form = Form("Restriction Cloning", "This window is the vector.")
        let v1 = form.popup("Vector enzyme", shown)
        let v2 = form.popup("and", shown)
        let insert = form.popup("Insert", names(inserts))
        let i1 = form.popup("Insert enzyme", all, selected: 0)
        let i2 = form.popup("and", all, selected: 0)
        let reversed = form.check("", "Insert reversed")
        form.note("Pick the same enzymes on both sides for matching ends.")
        // Start the insert enzymes on the vector's.
        let syncInsert = {
            if let a = v1.titleOfSelectedItem { i1.selectItem(withTitle: a) }
            if let b = v2.titleOfSelectedItem { i2.selectItem(withTitle: b) }
        }
        syncInsert()
        form.run(in: window, ok: "Open Product") { [weak self] in
            let ins = inserts[insert.indexOfSelectedItem]
            self?.product("Restriction Cloning") {
                oc_restriction_clone(d.handle, v1.titleOfSelectedItem ?? "", v2.titleOfSelectedItem ?? "", ins.handle,
                                     i1.titleOfSelectedItem ?? "", i2.titleOfSelectedItem ?? "", reversed.state == .on,
                                     $0, $1)
            }
        }
    }

    @objc func goldenGate(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let parts = others()
        guard !parts.isEmpty else { tell("Golden Gate Assembly", "Open the parts in other windows first."); return }
        let form = Form("Golden Gate Assembly", "This window is the vector; order and orientation come from the overhangs.")
        let enzymes = ["BsaI", "BsmBI", "BbsI", "SapI", "PaqCI", "Esp3I", "BtgZI"]
        let enzyme = form.popup("Enzyme", enzymes)
        let boxes = form.checks("Parts", names(parts))
        form.run(in: window, ok: "Open Product") { [weak self] in
            let chosen: [OpaquePointer?] = zip(parts, boxes).filter { $0.1.state == .on }.map { $0.0.handle }
            self?.product("Golden Gate Assembly") {
                oc_golden_gate(d.handle, chosen, Int32(chosen.count), enzymes[enzyme.indexOfSelectedItem], $0, $1)
            }
        }
    }

    @objc func gibson(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let frags = others(linearOnly: true)
        guard !frags.isEmpty else { tell("Gibson Assembly", "Open the linear fragments in other windows first."); return }
        let shown = Array(Set(d.visibleSiteEnzymes)).sorted()
        let form = Form("Gibson Assembly", d.isCircular ? "This window is the vector, opened with one or two enzymes."
                                                        : "This window is the linear vector.")
        let v1 = form.popup("Vector enzyme", d.isCircular ? shown : ["(linear)"])
        let v2 = form.popup("and", d.isCircular ? shown : ["(linear)"])
        let boxes = form.checks("Fragments", names(frags))
        let overlap = form.number("Minimum overlap (bp)", 15)
        form.run(in: window, ok: "Open Product") { [weak self] in
            let chosen: [OpaquePointer?] = zip(frags, boxes).filter { $0.1.state == .on }.map { $0.0.handle }
            self?.product("Gibson Assembly") {
                oc_gibson(d.handle, v1.titleOfSelectedItem ?? "", v2.titleOfSelectedItem ?? "", chosen,
                          Int32(chosen.count), Int32(overlap.number(or: 15)), $0, $1)
            }
        }
    }

    @objc func overhangCloning(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        guard !d.isCircular else {
            tell("TA, GC and TOPO Cloning", "Open the linearized vector (without its overhangs) as a linear sequence.")
            return
        }
        let inserts = others(linearOnly: true)
        guard !inserts.isEmpty else { tell("TA, GC and TOPO Cloning", "Open the PCR product in another window first."); return }
        let form = Form("TA, GC and TOPO Cloning", "Into \(d.name), the linearized vector.")
        let insert = form.popup("PCR product", names(inserts))
        let method = form.popup("Method", ["TA (3′ T overhangs)", "GC (3′ C overhangs)", "Blunt (TOPO blunt)"])
        let reversed = form.check("", "Insert reversed")
        form.run(in: window, ok: "Open Product") { [weak self] in
            let ins = inserts[insert.indexOfSelectedItem]
            let overhang: CChar = [CChar(UInt8(ascii: "T")), CChar(UInt8(ascii: "C")), 0][method.indexOfSelectedItem]
            self?.product("TA, GC and TOPO Cloning") {
                oc_overhang_clone(d.handle, ins.handle, overhang, reversed.state == .on, $0, $1)
            }
        }
    }

    @objc func overlapExtension(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        guard !d.isCircular else { tell("Overlap Extension PCR", "This joins linear fragments; this sequence is circular."); return }
        let frags = others(linearOnly: true)
        guard !frags.isEmpty else { tell("Overlap Extension PCR", "Open the other fragments in other windows first."); return }
        let form = Form("Overlap Extension PCR", "Joined from this window by their shared ends.")
        let boxes = form.checks("Fragments", names(frags))
        let overlap = form.number("Minimum overlap (bp)", 15)
        form.run(in: window, ok: "Open Product") { [weak self] in
            var chosen: [OpaquePointer?] = [d.handle]
            for (frag, box) in zip(frags, boxes) where box.state == .on { chosen.append(frag.handle) }
            self?.product("Overlap Extension PCR") {
                oc_overlap_extension(chosen, Int32(chosen.count), Int32(overlap.number(or: 15)), $0, $1)
            }
        }
    }

    @objc func insertionByPcr(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let inserts = others()
        guard !inserts.isEmpty else {
            tell("Insertion by PCR", "Open the insert in another window, with the part to insert selected.")
            return
        }
        let site = d.selectionLength > 0 ? "Replaces \(d.selectionStart + 1)–\(d.selectionStart + d.selectionLength)."
                                         : "Inserts at the caret (after base \(d.selectionStart))."
        let form = Form("Insertion by PCR", site)
        let insert = form.popup("Insert", names(inserts))
        let tm = form.number("Annealing Tm (°C)", 60)
        let overlap = form.number("Overlap with vector (bp)", 20)
        form.note("The insert's selection is amplified, or all of it when nothing is selected.")
        form.run(in: window, ok: "Open Construct") { [weak self] in
            let ins = inserts[insert.indexOfSelectedItem]
            var primers: UnsafeMutablePointer<CChar>?
            let out = CoreDocument()
            var err: UnsafeMutablePointer<CChar>?
            if oc_insertion_by_pcr(d.handle, ins.handle, tm.number(or: 60), Int32(overlap.number(or: 20)), out.handle,
                                   &primers, &err) {
                SequenceDocument.open(out)
                showText("Insertion Primers", "Both primers are also on the construct.", takeString(primers), in: nil)
            } else {
                showError("Insertion by PCR", err, in: self?.window)
            }
        }
    }

    @objc func mutagenesis(_ sender: Any?) {
        guard let d = doc, d.hasSelection else {
            tell("Mutagenesis", "Select the bases to change, or place the caret to insert.")
            return
        }
        let form = Form("Mutagenesis", d.selectionLength > 0
                        ? "Replace \(d.selectedBases.prefix(30))\(d.selectionLength > 30 ? "…" : "") (leave empty to delete)."
                        : "Insert at the caret.")
        let bases = form.text("New bases", "", width: 280)
        let tm = form.number("Annealing Tm (°C)", 60)
        form.run(in: window, ok: "Open Mutant") { [weak self] in
            self?.product("Mutagenesis") { oc_mutagenesis(d.handle, bases.stringValue, tm.number(or: 60), $0, $1) }
        }
    }

    @objc func cloningHistory(_ sender: Any?) {
        guard let d = doc else { return }
        let steps = (0..<Int(oc_document_history_count(d.handle))).map {
            String(cString: oc_document_history(d.handle, Int32($0)))
        }
        showText("Cloning History", steps.isEmpty ? "No cloning steps recorded for this sequence." : "Oldest first.",
                 steps.joined(separator: "\n"), in: window)
    }

    // MARK: Align

    private func align(_ title: String, candidates: [CoreDocument]) {
        guard let d = doc, !d.isEmpty else { return }
        guard !candidates.isEmpty else {
            tell(title, "Open the read or sequence (an .ab1 trace, FASTA or other file) in another window first.")
            return
        }
        let form = Form(title, "Aligned to \(d.name) on both strands, across the origin.")
        let pick = form.popup("Sequence", names(candidates))
        form.run(in: window, ok: "Align") { [weak self] in
            let other = candidates[pick.indexOfSelectedItem]
            var info = OCAlignmentInfo()
            var err: UnsafeMutablePointer<CChar>?
            guard let diffs = oc_align_read(d.handle, other.handle, &info, &err) else {
                showError(title, err, in: self?.window)
                return
            }
            let text = takeString(diffs).split(separator: "\n").map { (line: Substring) -> String in
                let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
                guard f.count == 3 else { return String(line) }
                return "\(f[0])\t\(f[1].isEmpty ? "–" : f[1]) → \(f[2].isEmpty ? "–" : f[2])"
            }.joined(separator: "\n")
            _ = oc_document_show_read(d.handle, other.handle, nil)
            let end = Int(info.ref_start + info.ref_length)
            let summary = "\(info.reverse ? "Bottom" : "Top") strand, \(info.ref_start + 1)–\(end), "
                + "\(String(format: "%.1f", info.identity * 100))% identity, \(info.diff_count) difference"
                + "\(info.diff_count == 1 ? "" : "s"). Shown under the sequence."
            showText(title, summary, text, in: self?.window)
        }
    }

    @objc func alignRead(_ sender: Any?) { align("Align Read", candidates: others(linearOnly: true)) }

    @objc func compareSequences(_ sender: Any?) { align("Compare", candidates: others()) }

    @objc func showAllReads(_ sender: Any?) {
        guard let d = doc, !d.isEmpty else { return }
        let reads = others(linearOnly: true)
        let shown = reads.filter { oc_document_show_read(d.handle, $0.handle, nil) }.count
        tell("Show All Reads", "Showing \(shown) of \(reads.count) reads under the sequence.")
    }

    @objc func hideReads(_ sender: Any?) {
        guard let d = doc else { return }
        oc_document_clear_reads(d.handle)
    }

    // MARK: View

    @objc func setFrames(_ sender: NSMenuItem) {
        doc?.frames = sender.tag
    }

    @objc func mapDisplay(_ sender: Any?) {
        guard let d = doc else { return }
        let form = Form("Map Display", "Exports follow these settings.")
        let enzymes = form.check("Show", "Enzymes", on: d.mapShowsEnzymes)
        let primers = form.check("", "Primers", on: d.mapShowsPrimers)
        let types = Array(Set(d.features.filter { $0.drawn }.map { $0.type })).sorted()
        let boxes = types.isEmpty ? [] : form.checks("Feature types", types)
        for (t, box) in zip(types, boxes) { box.state = d.mapHiddenTypes.contains(t) ? .off : .on }
        form.run(in: window, ok: "Apply") {
            d.mapShowsEnzymes = enzymes.state == .on
            d.mapShowsPrimers = primers.state == .on
            d.mapHiddenTypes = Set(zip(types, boxes).filter { $0.1.state == .off }.map { $0.0 })
        }
    }
}

// MARK: - Printing

/// The printed pages from the shared print layout, laid out at the paper's
/// printable size: the map, then the sequence in whole lines.
final class PrintView: NSView {
    private let document: CoreDocument
    private let page: CGSize
    private let count: Int

    init(document: CoreDocument, page: CGSize) {
        self.document = document
        self.page = page
        count = max(1, Int(oc_print_page_count(document.handle, page.width, page.height, measureText, nil)))
        super.init(frame: NSRect(x: 0, y: 0, width: page.width, height: page.height * CGFloat(count)))
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override var isFlipped: Bool { true }

    override func knowsPageRange(_ range: NSRangePointer) -> Bool {
        range.pointee = NSRange(location: 1, length: count)
        return true
    }

    override func rectForPage(_ number: Int) -> NSRect {
        NSRect(x: 0, y: page.height * CGFloat(number - 1), width: page.width, height: page.height)
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        let first = max(0, Int(dirtyRect.minY / page.height))
        let last = min(count - 1, Int((dirtyRect.maxY - 1) / page.height))
        guard first <= last else { return }
        for i in first...last {
            guard let scene = oc_print_page(document.handle, page.width, page.height, Int32(i), measureText, nil) else {
                continue
            }
            ctx.saveGState()
            ctx.translateBy(x: 0, y: page.height * CGFloat(i))
            drawScene(scene, in: ctx)
            ctx.restoreGState()
            oc_scene_free(scene)
        }
    }
}
