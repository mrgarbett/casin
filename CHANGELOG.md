# Changelog

Versions follow [Semantic Versioning](https://semver.org). Until 1.0, minor
versions may change file handling, menus and the C API.

## Unreleased

- **Mac Quick Look:** the map preview (Space on a .gb or .dna file in Finder) is now drawn as vectors instead of a bitmap, so it is crisp at any size and on Retina screens.
- **macOS:** File ▸ Open Recent; the files open at the last quit reopen when the app is launched with nothing to open; ⌘1 to ⌘9 switch window tabs.
- **macOS:** hovering a feature or an enzyme site on the map lightly highlights it in the sequence view, and the other way round.
- **macOS:** in the sequence view the arrow keys move the caret (by a base, or a line up and down), Shift+arrows grow the selection and Escape clears it.
- **Sequence view:** double-clicking an amino acid selects the whole translation: the open reading frame from its start codon through its stop, or the CDS feature under a feature's translation row (macOS; the Linux view follows). An amino acid lights up with the selection whenever its codon is selected, so a clicked codon and a double-clicked frame both show what they took.
- **macOS:** the app now needs macOS 26 Tahoe or later (built with Xcode 26). Support for macOS 13 to 15 and the plain, non-glass layout were dropped.
- **Focus:** the menus now show the core cloning tools only (primers, PCR, digest and gel, insertion by PCR, Gibson) plus opening, saving and editing. The other tools from the first alpha are hidden until each gets its own tested step; turn on **Experimental Tools** (main menu on Linux, the Restricto menu on the Mac) to show them again. Nothing was removed.
- **Find (Mac):** ⌘F also finds features, primers and enzymes by name, ignoring case ("attB1", "BglII"), and selects them. Name matches come first, then bases when the query is also DNA; pressing Return again steps through them all.
- **Enzymes (Mac):** the Enzymes list tags enzymes that cut inside a feature with the feature's name (the tooltip lists them all), so you can pick sites that leave your insert or resistance gene whole. "source" features don't count.
- **macOS:** Go to Position (⌘L) also takes a range, `100..500` or `100-500`, and selects it; on a circular sequence a range can run across the origin (`9000..200`).
- **Version:** the version now carries the commit it was built from, e.g. `0.1.0-alpha+1a2b3c4` (About box, `restricto --version`, `oc_version()`).
- **Enzymes:** with **On map only** on, unticking an enzyme no longer removes it from the list, so it can be ticked again.
- **Enzymes:** click one enzyme, then shift-click a second, on the map or in the sequence, to select the bases between their cuts (forward from the first on a circular sequence).
- **Mac:** Window ▸ New Window (⌘⇧N) opens a second window on the same file, e.g. the map in one and the sequence in the other. Both show the same sequence, selection and edits; the file stays open until its last window closes.
- **Geneious files:** the editing-history records Geneious exports as features ("nonstandard type: Editing History Insertion/Deletion/Replacement") are no longer drawn on the map or in the sequence. They stay in the file and are saved back unchanged.
- **Enzymes (Mac):** right-click an enzyme site on the map or in the sequence for **Select to** (the bases between its cut and another site's), **Hide** the enzyme, or **Digest with** it (opens Digest and Gel with that enzyme picked).
- **Enzymes (Mac):** a selection made that way names its fragment (e.g. "BamHI–XbaI: 1,234 bp") in the status and the selection badge, with a **Digest and Gel…** button beside it; Cloning ▸ Digest and Gel… opens with those two enzymes picked.
- **Selection:** shift-click a feature or primer, on the map or in the sequence, to grow the selection to cover it (the shorter way round on a circular sequence).

## 0.1.0-alpha (2026-10-27)

First alpha.

- **Viewing:** GenBank, FASTA, SnapGene `.dna` and EMBL files (multi-record files open one window per record); circular and linear maps; a double-strand sequence view with three- or six-frame translation; map display options; printing and PDF export.
- **Editing:** typing, paste, delete, reverse complement, set origin, features and primers, undo and redo; saving GenBank and `.dna`.
- **Enzymes:** the REBASE commercial set, cut-count filters, Choose Enzymes, enzyme sets (Type IIS, per supplier, your own).
- **Primers:** checks (Tm, GC, clamp, dimers, hairpins), pair design for a selection or a flanking window, adding a primer by sequence.
- **Cloning:** digest and virtual gel, PCR, restriction cloning, Gibson/HiFi/In-Fusion, Golden Gate, TA/GC/TOPO, overlap extension PCR, insertion by PCR, site-directed mutagenesis, cloning history.
- **Verification:** Sanger `.ab1` traces, read alignment shown under the sequence, comparing two sequences, ORF finding, protein properties, folder search.
- **macOS:** the Mac app has menus for the same tools through the C API.

Not yet: Dam/Dcm methylation, Gateway cloning, NCBI fetch, multiple sequence alignment.
