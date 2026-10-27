# OpenClone

An open-source, native desktop plasmid viewer for Linux and macOS. It opens
GenBank, FASTA and SnapGene `.dna` files and shows a circular (or linear) map with
features and restriction sites, next to a double-strand sequence view with
translations.

*OpenClone is a working name. It isn't affiliated with any commercial product.*

- **Linux:** GTK4 + libadwaita, drawn with cairo/Pango.
- **macOS:** Swift + AppKit, drawn with CoreGraphics.
- **Shared:** a plain C++17 core (no GUI toolkit) and a shared layout layer, so the
  map and sequence view look and behave the same on both systems.

No Electron, no web stack, no Qt.

![Linux window](docs/screenshot-linux.png)

## Layout

```
src/core/     openclone_core  sequence model, GenBank/FASTA/.dna readers, enzymes, Tm, I/O (C++17, std only)
src/view/     openclone_view  map + sequence-view layout, hit testing, Document model (C++17, std only)
              cairo_render.*  draws layouts with cairo/Pango (Linux app, PNG/SVG/PDF export)
src/capi/     openclone_capi  C API over core + view, used by the Swift app
src/gtk/      OpenClone for Linux (GTK4 + libadwaita)
macos/        OpenClone for macOS (Swift + AppKit), built via Package.swift
tests/        core_tests, view_tests, capi_tests (CTest)
samples/      synthetic demo plasmid (.gb + .dna) from scripts/make_demo_plasmid.py
packaging/    .desktop file, MIME types, AppStream metainfo, icon, macOS Info.plist
```

## Building on Linux (Debian/Ubuntu)

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
                 libgtk-4-dev libadwaita-1-dev libcairo2-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/gtk/openclone samples/pOC-demo.dna
sudo cmake --install build      # binary, .desktop file, MIME types, icon
```

Needs GTK ≥ 4.12 and libadwaita ≥ 1.5 (Ubuntu 24.04, Debian 13, Fedora 40 or newer).
Fedora: `sudo dnf install gcc-c++ cmake ninja-build gtk4-devel libadwaita-devel`.

## Building on macOS

```sh
xcode-select --install                 # or a full Xcode, 15 or newer
scripts/build_macos_app.sh             # -> build/macos/OpenClone.app
open build/macos/OpenClone.app
```

You can also open `Package.swift` in Xcode, or run `swift build`. The C++ core and
its tests still build with CMake on macOS:

```sh
brew install cmake ninja
cmake -S . -B build -G Ninja -DOPENCLONE_FRONTEND=none && cmake --build build
ctest --test-dir build
```

## Headless checks

```sh
# Map image without a display (.png, .svg or .pdf). Linux:
./build/src/gtk/openclone --render-map map.png --enzymes all samples/pOC-demo.dna
# Window screenshot (needs a display; Xvfb works):
xvfb-run -a env GSK_RENDERER=cairo ./build/src/gtk/openclone \
    --screenshot window.png --size 1440x900 --select 170-892 samples/pOC-demo.dna
# macOS map image:
build/macos/OpenClone.app/Contents/MacOS/OpenClone --render-map map.png samples/pOC-demo.dna
```

Options: `--size WxH`, `--enzymes 1|2|3|all` (cutter filter), `--select A-B`
(1-based), `--dark`.

## What works in this milestone

- Open GenBank, FASTA and SnapGene `.dna` files (drag and drop, command line, Finder or
  the file manager). One window per document.
- Circular map with feature arrows in lanes, names along the arc (or as callouts when
  they don't fit), enzyme labels in spread-out columns with leader lines, a tick ring,
  and a selection wedge. Linear map for linear sequences.
- Sequence view: wrapped in multiples of 10 bp, enzyme labels, top and complement
  strands, ruler, feature lanes, and amino acids under CDS features (honors
  `/codon_start`). Shows the cut marker for the active enzyme site.
- Selection by click, drag or shift-click, kept in sync between the map and the
  sequence. Status bar shows length, topology, selection, GC% and Tm for 8–100 bp.
- Features and Enzymes sidebar: per-enzyme show/hide, cut counts, and a unique / ≤2 /
  ≤3 / all cutters filter.
- Find on both strands (IUPAC codes allowed). Copy and Copy Reverse Complement.
- Save As GenBank, Export FASTA, Export Map (PNG/SVG/PDF on Linux, PNG/PDF on macOS).

## Next steps

Not in this milestone:

- Sequence editing (insert, delete, and features that follow the edits)
- Cloning simulation: restriction–ligation, Gibson assembly, Golden Gate
- Primer design (we already read primers from `.dna` files, but don't draw them yet)
- Sanger trace viewing (`.ab1`)
- Writing `.dna` files
- Enzymes that cut twice per site, methylation sensitivity, a user enzyme set editor

## Credits and ground rules

- Restriction enzyme data comes from **REBASE** (https://rebase.neb.com); see
  [docs/REBASE.md](docs/REBASE.md).
- Melting temperatures use the SantaLucia (1998) unified nearest-neighbor parameters.
- The `.dna` reader is a clean-room implementation of the publicly documented packet
  format. No proprietary code, artwork, colors, help text or feature databases are
  used. The colors are our own, based on the GNOME palette.
- `samples/` holds only **synthetic** sequences generated by
  `scripts/make_demo_plasmid.py`. They are not real plasmids.

## License

GPL-3.0-or-later (see the SPDX headers).
