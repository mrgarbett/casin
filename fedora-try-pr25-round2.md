# Fedora try-out: PR #25, round 2

- Date: 2026-10-28
- Machine: Fedora 44, GNOME on Wayland, GTK 4.22.5, libadwaita 1.9.4, GCC 16.2.1
- Branch: `claude/project-thread-rbkvqv` at `3f56939` ("GTK: menu parity with the Mac app"), on top of the round-1
  commits `40267ac` and `05aba1d`
- Build: Release, no errors, **no compiler warnings** (`src/gtk/dialogs.cpp` was recompiled with 0 warnings;
  `src/core/genbank.cpp` is unchanged since round 1, when it also had 0).
- Tests: `ctest --test-dir build --output-on-failure`: 4/4 pass.

How it was tested: as in round 1, on a separate X display (Xvfb + xdotool, `GDK_BACKEND=x11`, own D-Bus,
`GDK_DEBUG=no-portals`) with real clicks, menus, typing and shortcuts, on a copy of `samples/pOC-demo.dna` and on
`samples/reference/pUC19-L09137.gb`. The clipboard was read back with a GTK 4 script (`Gdk.Clipboard.read_texture_async`).
No code was changed. The round-1 checks were not redone: nothing looked off, and the new GenBank/EMBL primer tests
in `ctest` pass.

## Results (NEW checks)

| Check | Result | Note |
|-------|--------|------|
| Menu layout: File items, Edit items, Sequence and Cloning submenus, View | ✅ | ☰: New Sequence…, Open…, Search a Folder…, Save, Save As…, Revert to Saved, Export FASTA…, Export Map…, Print… / Cut, Copy, Copy Reverse Complement, Copy Map Image, Paste, Select All, Find…, Go to Position… / Sequence ›, Cloning ›, Align › / View ›, Experimental Tools, About. |
| Sequence submenu | ✅ | Reverse Complement, Set Origin at Selection, Circular, Rename…, then Features › (Add/Edit/Delete Feature, Annotate Common Features, Find ORFs…, Protein Properties…) and Primers › (Add Primer…, Add Primer by Sequence…, Delete Primer…, Design Primers…, Check a Primer…). |
| Cloning submenu | ✅ | Digest and Gel… (Shift+Ctrl+G), Choose Enzymes… / PCR Product…, Insertion by PCR…, Gibson Assembly… / Restriction Cloning…, Golden Gate…, TA/GC/TOPO…, Overlap Extension PCR…, Mutagenesis…, Cloning History. |
| Compared with the Mac menus (`macos/Restricto/Tools.swift`) | ✅ | Same items and order, except two Mac items GTK doesn't have: **Enzyme Set…** and **Show All Reads**. Not part of the brief, listed for completeness. |
| Revert to Saved | ✅ | Greyed until a change. Deleted T-demo, ☰ > Revert to Saved asks "Revert to the saved file? Your changes to work.dna since it was last saved will be lost." [Cancel] [Revert]. Revert: T-demo is back, toast "Reverted to work.dna", "•" and undo cleared, the item greyed again. |
| Copy Map Image (Ctrl+Alt+C) | ✅ | The clipboard holds a 1800×1520 image of the light-theme map. Read back with GTK 4, not pasted into GIMP/LibreOffice (worth one manual paste on the desktop). |
| Go to Position… (Ctrl+L), 1500, Go | ✅ | Caret goes before base 1,500, the sequence scrolls to line 1,481, and the map marker moves there. See note A. |
| Rename… | ✅ | "pTEST-renamed" shows in the title and the map's centre label. Ctrl+Z gives back "work". |
| Annotate Common Features on pUC19-L09137.gb | ❌ | Adds 5 features, toast "Added 5 features: M13 fwd, M13 rev, lac operator, lac promoter, AmpR". **No ori**, which the brief expects. See problem 1. Undo removes all 5 in one step ("Undid annotate common features"). |
| Add Primer by Sequence… while bases are selected | ✅ | With 41–74 selected: the dialog opens, and `GTAAAACGACGGCCAGT` "binds the top strand at 379–395 (17 nt)" with warnings. Added. |
| Delete Primer… | ✅ | The dropdown lists "Primer 1 · 379–395 →" and "Primer 2 · 465–481 ←". Picking Primer 2 and Delete removes only it. |
| Check a Primer… | ✅ | Updates as you type. At `GAATTCGCGC`: 10 nt, GC 60%, Tm 35.1 °C, clamp 5 of last 5, run 2, warnings short/Tm/3′ G/C/self-dimer. At `GAATTCGCGCAAAAAGCGCGAATTCGG`: 27 nt, GC 52%, Tm 65.1 °C, clamp 3 of 5, run 5, warnings "Run of 5 identical bases", "Self-dimer with 10 pairs", "Hairpin with a 10 bp stem". |
| Group Bases in Tens (View) | ✅ | A gap after every 10 bases on both strands. Turning it off restores plain lines. |
| Ctrl+Shift+K edits the selected feature | ✅ | Clicking demoR in the panel selects 1,365–1,907. Ctrl+Shift+K opens Edit Feature for demoR. |
| Ctrl+Shift+G / Ctrl+D open Digest and Gel | ✅ | Both open it. |

## Problems (exact steps)

### 1. Annotate Common Features finds no origin on pUC19 ❌
1. `./build/src/gtk/restricto samples/reference/pUC19-L09137.gb` (no features).
2. ☰ > Sequence > Features > Annotate Common Features.
3. Toast: "Added 5 features: M13 fwd, M13 rev, lac operator, lac promoter, AmpR". No ori (and no lacZα).

Cause: the shared core library `src/core/featurelib.cpp` has only `p15A ori` and `f1 ori`. It has no pMB1/ColE1
("pUC") origin, which is the one pUC19 carries, so the Mac app gives the same result. The GTK menu code works. The fix is a
library entry (pUC ori, perhaps lacZα), not a GTK change.

### Note A. "Go to 1500" leaves the status bar at "position 1,499" (minor)
After Go to Position… 1500 the caret sits before base 1,500, as on the Mac (`select(start: base - 1, length: 0)`),
but the status bar shows "position 1,499" because it counts the bases before the caret
(`Document::statusText`). Reopening Ctrl+L then pre-fills 1500. It's consistent, but a user who typed 1500 sees 1,499.

## Terminal output (stderr)

Nothing from the app. The test display printed only its own DRI3/vulkan messages. The desktop window opened for you
in round 1 also exited (code 0) with empty stderr.
