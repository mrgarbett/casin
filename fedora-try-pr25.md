# Fedora try-out: PR #25

- Date: 2026-10-28
- Machine: Fedora 44, GNOME on Wayland, GTK 4.22.5, libadwaita 1.9.4, GCC 16.2.1
- Branch: `claude/project-thread-rbkvqv` at `05aba1d` (2 commits on `main` 49712ce: "GTK: set a feature's colour in
  Add/Edit Feature", "GenBank and EMBL: keep primers when saving")
- Build: Release, no errors. **No new compiler warnings**: `src/gtk/dialogs.cpp` and `src/core/genbank.cpp`
  were recompiled with 0 warnings.
- Tests: `ctest --test-dir build --output-on-failure`: 4/4 pass.

How it was tested: the GUI checks were run on a separate X display (Xvfb + xdotool, `GDK_BACKEND=x11`, own
D-Bus session, `GDK_DEBUG=no-portals`) with real clicks, typing, menus and the GTK colour chooser, on a copy of
`samples/pOC-demo.dna`. Files were reopened by passing the path to the running app (the same `open` path
as ☰ > Open…). Dark mode used `ADW_DEBUG_COLOR_SCHEME=prefer-dark`. No code was changed.

## Results

| # | Check | Result | Note |
|---|-------|--------|------|
| 1 | Primers survive GenBank: Save As `/tmp/x.gb`, reopen | ✅ | seq-fwd (2,761–2,784 →) and mcs-rev (1,083–1,102 ←) listed and drawn on the map. Features tab: 11 features, **one** "seq-fwd site". |
| 1 | `grep -A2 primer_bind /tmp/x.gb` | ✅ | The existing `seq-fwd site` feature, plus `/label="seq-fwd"` `/note="sequence: ACGTACAATGTGTTCGAAACAGGT"` and `complement(1083..1102)` `/label="mcs-rev"` `/note="sequence: CGGCTCTCTTTACACGGCTC"`. |
| 2 | Tailed primer `gaattcCCTAATAAGCCGGTATGACGG` "tail-test", Save As `/tmp/y.gb`, reopen | ✅ | Dialog: "Binds the top strand at 241–261 (21 nt), 6 nt 5′ tail". After reopening: "27 nt · Tm 59.7 °C · 241–261 →", tail `GAATTC` drawn before 241. File: `primer_bind 241..261`, `/note="sequence: GAATTCCCTAATAAGCCGGTATGACGG"`. See note A on the tail's case. |
| 3 | EMBL: Save As `/tmp/z.embl`, reopen | ✅ | All 3 primers written as `FT primer_bind` with their notes. After reopening seq-fwd, mcs-rev and tail-test are listed and drawn, tail intact. |
| 4 | Edit demoA: Colour row with colour button and Default | ✅ | Row shows "Colour · #62a0ea", a **Default** button and a swatch. The swatch opens the GTK colour chooser. |
| 4 | Pick purple, Save | ✅ | #9141ac: demoA turns purple on the map, the sequence bar and the panel swatch. |
| 4 | Ctrl+Z / Ctrl+Shift+Z | ✅ | Undo goes back to blue in one step ("Undid edit feature"). Redo brings the purple back. |
| 5 | Edit demoA, Default, Save | ✅ | Row turns "Type default", the Default button dims, the swatch is CDS blue. After Save demoA is blue everywhere. |
| 5 | While on Default, change Type to promoter | ✅ | Swatch turns promoter green, still "Type default". Cancelled afterwards. |
| 6 | Add Feature with a colour | ✅ | Selected 81–120, Ctrl+K, name "colour-new", picked orange (#ff7800), Add: orange on the map, in the sequence and in the panel. |
| 7 | Colour saved: Save As `/tmp/c.gb` and `/tmp/c.dna`, reopen each | ✅ | colour-new is orange after reopening both. GenBank stores it as `/ApEinfo_fwdcolor="#ff7800"` and `/ApEinfo_revcolor="#ff7800"`. |
| 8 | Dark mode: Edit Feature dialog | ✅ | The Colour row reads well: "Default" bold when it can be used and dimmed on the default, the #hex or "Type default" subtitle readable, the swatch framed. |

## Notes (not caused by this PR)

**A. Add Primer by Sequence drops a lowercase tail's case.**
1. With nothing selected, ☰ > Primers > Add Primer…, type `gaattcCCTAATAAGCCGGTATGACGG`, Add.
2. Save As `/tmp/y.gb`: the note reads `sequence: GAATTCCCTAATAAGCCGGTATGACGG` (all uppercase).

The case is lost before saving. `updatePrimerSeq` in `src/gtk/dialogs.cpp:1081` runs the entry through
`cleanBases()`, which uppercases. The new GenBank reader goes through `cleanPrimerBases()`, which keeps mixed
case, so a mixed-case primer from a `.dna` file should survive GenBank (not checked in the GUI). The tail itself is kept,
so check 2 passes. This only matters if lowercase tails are meant to be kept, as typed bases are in the sequence.

**B. Redo's toast showed late.** Right after Ctrl+Shift+Z the "Undid edit feature" toast was still on screen.
Redo does call `toast("Redid …")` (`src/gtk/window.cpp:1192`), so this is probably libadwaita queuing toasts.
This is old behaviour.

## Terminal output (stderr)

The app printed nothing on stderr during any check. The only lines came from the virtual display itself
(`MESA-EGL: warning: DRI3 error: Could not get DRI3 device`, `MESA: info: vulkan: No DRI3 support detected`).
