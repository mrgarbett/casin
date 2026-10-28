# Changelog

Versions follow [Semantic Versioning](https://semver.org). Until 1.0, minor
versions may change file handling, menus and the C API.

## Unreleased

- **Focus:** the menus now show the core cloning tools only (primers, PCR, digest and gel, insertion by PCR, Gibson) plus opening, saving and editing. The other tools from the first alpha are hidden until each gets its own tested step; turn on **Experimental Tools** (main menu on Linux, the Restricto menu on the Mac) to show them again. Nothing was removed.

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
