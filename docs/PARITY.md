# Feature parity checklist

The goal is to cover what people use a commercial plasmid editor for, built
clean-room. This list names capabilities in our own words, from public product
descriptions; it copies no text, artwork, colors, feature databases or sample
files from any commercial product.

IDs match the roadmap. Status: **done**, **partial**, or the milestone that
plans it (M2 Edit and save, M3 Clone, M4 Verify, M5 Parity breadth).

## Files and formats

| ID | Capability | Status |
| --- | --- | --- |
| | Open GenBank, FASTA and `.dna` | done |
| E5 | Save `.dna` (features, primers, description) | done on Linux; Mac UI pending |
| | Save GenBank, export FASTA | done |
| | Export the map as PNG, SVG, PDF | done |
| I4 | EMBL and multi-record GenBank | M5 |
| I3 | Fetch a sequence from NCBI by accession | M5 |
| I1 | Print the map and sequence | planned |

## Viewing

| ID | Capability | Status |
| --- | --- | --- |
| | Circular and linear maps with features and enzyme sites | done |
| | Double-strand sequence view with translations | done |
| | Linked selection, find on both strands, GC and Tm | done |
| E6 | Primers drawn on the sequence and the circular map | done |
| E7 | ORF finder and six-frame translation | done on Linux; Mac UI pending (C API has `oc_seq_layout_new_frames`) |
| X1 | Map display options (feature types, labels, colors) | M4 |
| X8 | Protein sequence files and properties | M5 |

## Editing

| ID | Capability | Status |
| --- | --- | --- |
| E1 | Undo and redo, unsaved-changes prompt | done on Linux; Mac UI pending |
| E2 | Type, paste, delete bases; features follow the edits | done on Linux; Mac UI pending |
| E3 | Add, edit and delete features | done on Linux; Mac UI pending |
| E4 | Reverse complement, set origin, topology, new from pasted text | done on Linux; Mac UI pending |
| E6 | Add and delete primers | done on Linux; Mac UI pending |
| X3 | Copy and paste keeps features and primers | done on Linux |
| X9 | Highlight changes against a parent sequence | M5 |

## Enzymes

| ID | Capability | Status |
| --- | --- | --- |
| | Common Type II and Type IIS enzymes from REBASE, cutter filters | done |
| Z1 | Cuts on both strands, offset cuts, two-cut enzymes | done |
| Z2 | Dam/Dcm methylation sensitivity | planned |
| Z3 | Saved enzyme sets, supplier filter | planned |
| X2 | Choose enzymes by criteria (cuts once in a region, spares the insert) | done on Linux; Mac UI pending |

## Cloning simulation

| ID | Capability | Status |
| --- | --- | --- |
| C1 | Restriction digest and virtual agarose gel | done on Linux; Mac UI pending |
| C2 | Restriction-ligation cloning | done on Linux; Mac UI pending |
| C3 | PCR product from two primers, with 5′ tails | done on Linux; Mac UI pending |
| C4 | Gibson, HiFi and In-Fusion assembly | done on Linux; Mac UI pending |
| C5 | Golden Gate assembly | done on Linux; Mac UI pending |
| C6 | Cloning history saved with each construct | steps as text, kept in GenBank and `.dna`; Mac UI pending. Graphical history and re-running steps are later work |
| C7 | Gateway BP and LR | M5 |
| C8 | Site-directed mutagenesis | done on Linux (back-to-back primers); Mac UI pending |
| X4 | TA, TOPO and GC cloning | M5 |
| X5 | Overlap-extension PCR, insertion by PCR | M5 |
| X6 | Site-specific and homologous recombination | M5 |

## Primers

| ID | Capability | Status |
| --- | --- | --- |
| | Tm of a selection (SantaLucia 1998) | done |
| P1 | Primer checks: GC, Tm, 3′ clamp, runs, hairpins, self- and pair dimers | done on Linux; Mac UI pending |
| P2 | Automatic primer pair design | exact-ends design done on Linux; search window and Mac UI pending |
| P3 | Find where a primer binds, with mismatches | done on Linux (not across the origin yet); Mac UI pending |

## Verification

| ID | Capability | Status |
| --- | --- | --- |
| V1 | Sanger `.ab1` traces | done on Linux (chromatogram, quality, click to select); Mac UI pending |
| V2 | Align a sequence to the reference | done on Linux (one read at a time, differences listed); Mac UI pending |
| V3 | Traces aligned to the reference | M4 |
| V4 | Detect common features | M5 |
| X7 | Multiple sequence alignment | M5 |
| X10 | Search a folder of sequence files | M5 |
