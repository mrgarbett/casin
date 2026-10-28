# Fedora viewer cut-off check

- Date: 2026-10-28
- Machine: Fedora 44 (kernel 7.2.11), GNOME on Wayland, GTK 4.22.5, libadwaita 1.9.4, GCC 16.2.1
- Code: `main` at `49712ce` ("macOS: viewer cut-off (⌘F toggle and action pill fixes) (#22)")
- Build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`: builds, no errors
- Tests: `ctest --test-dir build --output-on-failure`: 4/4 pass (core_tests, view_tests, capi_tests, render_map_smoke)

How it was tested: dark mode was checked on the GNOME desktop by a person. The other GUI checks ran on
a separate X display (Xvfb + xdotool, `GDK_BACKEND=x11`, own D-Bus session, `GDK_DEBUG=no-portals`)
with real mouse drags, typing, menu clicks and dialogs, on a copy of `samples/pOC-demo.dna`.
The core logic was also checked with a small program linked against `librestricto_core`/`librestricto_view`.
No source code was changed.

## Results

| # | Check | Result | Note |
|---|-------|--------|------|
| 1 | Map: features, enzyme labels, position ticks | ✅ | |
| 1 | Sequence: both strands, features and primers under the bases | ✅ | Translations show too. Primers draw as arrows above the top strand. |
| 2 | Drag on the map: status shows positions, length, GC | ✅ | "selected 301–793 (493 bp) · GC 54.6%". The sequence scrolls to the selection. |
| 2 | Drag in the sequence: status shows positions, length, GC, Tm | ✅ | "selected 361–380 (20 bp) · GC 55.0% · Tm 58.0 °C". Tm only shows for 8–100 bp, by design (`src/view/document.cpp:600`, same cut-off as the Mac badge). |
| 3 | Typing `acgt` stays lowercase | ✅ | Both strands lowercase (`acgt` / `tgca`). Features after the insert shift by 4. |
| 3 | Undo / redo | ✅ | The "•" modified mark and the toolbar arrows update. |
| 3 | Save as GenBank, reopen: lowercase kept | ✅ | `acgtTGCGAC` at 391 in the file and after reopening. |
| 3 | Save as GenBank keeps the rest of the document | ❌ | **Primers are lost without a warning.** See problem 1. |
| 4 | Dam/Dcm: fully blocked enzymes draw greyed on the map | ✅ | StuI is greyed on pOC-demo. XbaI and ClaI are greyed on a test plasmid with GATC-overlapping sites. **The brief's example doesn't work on pOC-demo**: XbaI has 2 sites with only 1 Dam-blocked (so it is not greyed, correctly), and ClaI doesn't cut. No sample in `samples/` has a fully blocked XbaI or ClaI. `samples/reference/lambda-NC_001416.gb` has a fully blocked XbaI and BclI. |
| 5 | Add a feature from a selection | ✅ | Start/end filled in from the selection. |
| 5 | Edit a feature: name, type | ✅ | Map, sequence and panel redraw with the new name and type colour. |
| 5 | Edit a feature: colour | ❌ | **No colour control anywhere in the GTK app.** See problem 2. |
| 5 | Delete a feature | ✅ | From the Edit dialog's Delete Feature button. |
| 6 | Design Primers for Selection (~500 bp): two primer arrows | ✅ | 998–1,500: work-fwd 26 nt / work-rev 27 nt, both Tm 60.5 °C, warnings listed (self-dimer, hairpin). |
| 6 | Add a primer by its sequence | ✅ | With a 6 nt 5′ tail: "Binds the top strand at 241–264 (24 nt), 6 nt 5′ tail", added and drawn. Only reachable when nothing is selected (problem 4). |
| 7 | Cloning > PCR Product with the two primers | ✅ | Dialog says "503 bp product, 3 features carried over". Opens "work PCR 503 bp", linear, in a new window. |
| 8 | Digest and Gel: EcoRI + another enzyme, ladder and bands | ✅ | BamHI + EcoRI → 3,168 + 24 bp. Ladder, Uncut and Digest lanes. The 24 bp band sits at the bottom, below the 1 kb ladder's range. |
| 9 | Open `samples/pOC-demo.gb` | ✅ | 3,188 bp, 11 features. |
| 9 | Open `samples/pBAD30-plain.gb` | ✅ | 4,923 bp, no features (as named). |
| 9 | Export Map as PNG | ✅ | Ctrl+E. Default name `<name>-map.png`, 1800×1520 PNG. |
| 9 | Export Map as PDF | ✅ | Same dialog, `.pdf` extension: 1-page PDF, 900×760 pt, fonts embedded. |
| 10 | Dark mode: switching GNOME style live | ✅ | Seen on the desktop: map, sequence, panel and header bar follow dark → light → dark with no restart. |

## Problems (exact steps)

### 1. Saving as GenBank drops all primers, without warning ❌
1. `./build/src/gtk/restricto samples/pOC-demo.dna`. The Primers tab lists seq-fwd and mcs-rev, and both are drawn on the map.
2. ☰ > Save As…, name it `/tmp/x.gb`, Save. The toast says "Saved x.gb". No warning.
3. ☰ > Open… `/tmp/x.gb`.
4. Result: the Primers tab is empty and the seq-fwd/mcs-rev arrows are gone from the map. Only the `seq-fwd site` primer_bind *feature* is left. `grep mcs-rev /tmp/x.gb` finds nothing.

Expected: the primers are kept (for example as `primer_bind` features with the primer's bases), or the user is warned before saving.
Cause: `src/core/genbank.cpp` has no primer handling at all (only `snapgene.cpp` reads and writes primers).
The same happens for primers designed or added in the session. The core shows it too: load pOC-demo.dna (2 primers), `saveSequenceFile(..., FileFormat::GenBank)`, reload → 0 primers.

### 2. No way to set a feature's colour ❌
1. Open `samples/pOC-demo.dna`, select some bases, ☰ > Features > Add Feature…: Name, Type, Strand, Start, End and Note only.
2. Add it, then click its pencil in the Features panel (Edit Feature): the same fields plus Delete Feature. No colour.
3. Clicking the colour swatch in the Features panel does nothing.

The core supports it (`oc_document_set_feature_color`, `Feature::color`), and the Mac Edit Feature sheet sets it (`macos/Restricto/Tools.swift:456`). Colour only changes with the type.

### 3. GenBank round-trip turns unstranded features into forward ones (minor)
1. Open `samples/pOC-demo.dna`. In the Features panel "origin tag" reads "misc_feature · 3,159–40" with no arrow.
2. Save As `/tmp/x.gb`, reopen.
3. It now reads "misc_feature · 3,159–40 · →" and draws with an arrowhead on the map and under the sequence.

GenBank has no "no strand" location, so this may be accepted. It also explains why `samples/pOC-demo.gb` shows origin tag as forward.

### 4. Add Primer by sequence is hidden while anything is selected (minor, usability)
1. Select bases (e.g. drag 998–1,500 on the map).
2. ☰ > Primers > Add Primer…: the dialog is "Binds the selected 503 bp" with only Name and Direction. There's no way to type a sequence, and nothing warns that a 503-nt primer is unusual.
3. Click once in the sequence (caret, no selection), then Add Primer… again: now it is "Add Primer by Sequence".

By design (`src/gtk/window.cpp:1302`), but hard to discover. There is no separate "Add Primer by Sequence…" menu item.

### 5. Smaller things
- **Digest and Gel**: the enzyme list has no search box. Reaching EcoRI means scrolling about 50 rows.
- **Primers panel**: the position subtitle wraps mid-range ("… · 998–" / "1,023 · →").
- **Features panel**: blank when a sequence has no features (pBAD30-plain), with no empty-state hint.
- **`--select A-B` with `--screenshot`** highlights the range on the map but doesn't scroll the sequence pane to it. A real drag does scroll it. This affects headless screenshots only.
- **Digest**: a single cut on a circle reads "1,042–1,041 · BamHI (5′ 4) to BamHI (5′ 4)". Correct, but end-before-start looks odd.

## Terminal output (stderr)

The real run on the GNOME desktop (`./build/src/gtk/restricto samples/pOC-demo.dna`) printed **nothing** to
stderr and exited with code 0.

Build warnings (GCC 16.2.1, Release), from `src/view/seqlayout.cpp` and `src/view/maplayout.cpp`, inlined
from `std::vector<oc::view::Point>::_M_assign_aux` (a vector of `Point` assigned from a range):

```
/usr/include/c++/16/bits/stl_algobase.h:424:32: warning: argument 1 null where non-null expected because argument 3 is nonzero [-Wnonnull]
  424 |               __builtin_memmove(_GLIBCXX_TO_ADDR(__result),
/usr/include/c++/16/bits/stl_algobase.h:424:32: note: in a call to built-in function 'void* __builtin_memmove(void*, const void*, long unsigned int)'
```

(Twice per file. Probably a GCC 16 false positive, but `-Werror` builds would fail.)

The test display (Xvfb) printed only setup messages, not app errors:
`MESA-EGL: warning: DRI3 error: Could not get DRI3 device`, `MESA: info: vulkan: No DRI3 support detected`,
and `Gtk-WARNING: Unable to acquire the address of the accessibility bus` (private D-Bus session).

## Mac viewer features missing on Fedora

The GTK window does have a right-hand side panel already (Features / Enzymes / Primers tabs, toggled from the
header bar), so this is not the bare single window the brief describes. The Mac layout from rounds 1–7 is still missing:

- **Tabs**: every opened file, PCR product or reopened file gets its own top-level window.
- **Folder sidebar** (`FolderBrowserController`): "Not Saved" documents, files in a folder and its subfolders,
  a search field, red unsaved dots, File > Show Folder in Sidebar…. GTK only has ☰ > Search a Folder…, a dialog.
- **Right panel / Inspector as on Mac**: the vertical glass pill, the Sequence pane in the panel when the map
  sits beside it, View > Hide Inspector (⌥⌘I). GTK's panel is a plain libadwaita view switcher.
- **Glass pills**: the split status pill and action pill, and the selection badge that follows the mouse while
  dragging (positions, length, Tm). GTK shows this in a flat status bar at the bottom.
- **Centred title view**: GTK shows title + "3,188 bp · circular · file" in the header bar, which is close.
- **Annotate Common Features**: not in the GTK menus, although the core has it
  (`oc_document_annotate_common`, `src/core/featurelib.cpp`).
- **Go to Position…** (⌘L).
- **Copy Map Image** (⌥⌘C). GTK has Export Map… only.
- **View > Hide Map / Stack Map Above Sequence / Group Bases in Tens.** GTK's View menu has
  Show Complement Strand and Translate Frames only.
- **Feature colour editing** (see problem 2).
- **Quick Look preview** (Mac-only by nature; the Linux counterpart would be a thumbnailer).
- **Empty-window drop target** ("Open a file from the sidebar, or drop one here"). Not checked whether GTK accepts drops.

## Notes for whoever re-runs this

- `GDK_DEBUG=no-portals` was needed only because the test display had its own D-Bus session. Normally GTK uses
  the GNOME portal file chooser.
- In that no-portal chooser, typing a full path and pressing Return or Open did not open the file, but
  double-clicking the file row did. This is probably specific to the test setup, not checked on the desktop.
- The uninstalled dev build lists `.gb` files as type "Program" in the file chooser, because the MIME
  definitions in `packaging/linux/` aren't installed.
