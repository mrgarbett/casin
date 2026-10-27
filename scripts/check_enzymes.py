#!/usr/bin/env python3
"""Cross-check src/core/enzymes.cpp against REBASE data.

REBASE data is read from Biopython's Bio.Restriction.Restriction_Dictionary,
which is generated from REBASE's EMBOSS export (emboss_e). Usage:

    python3 scripts/check_enzymes.py [path/to/Restriction_Dictionary.py]

Without an argument, Biopython must be importable.
"""
import pathlib
import re
import runpy
import sys

root = pathlib.Path(__file__).resolve().parent.parent
src = (root / "src/core/enzymes.cpp").read_text()
ours = re.findall(r'\{"(\w+)", "([A-Z]+)", (-?\d+), (-?\d+)(?:, (-?\d+), (-?\d+), true)?\}', src)

if len(sys.argv) > 1:
    rest = runpy.run_path(sys.argv[1])["rest_dict"]
else:
    from Bio.Restriction.Restriction_Dictionary import rest_dict as rest

bad = 0
for name, site, top, bottom, top2, bottom2 in ours:
    top, bottom = int(top), int(bottom)
    r = rest.get(name)
    if r is None:
        print(f"{name}: not in REBASE data"); bad += 1; continue
    fst5, fst3, scd5, scd3, rsite = r["charac"]
    L = len(rsite)
    # REBASE/EMBOSS: fst5 = top cut from the site start; fst3 = bottom cut
    # relative to the site end (negative = inside the site).
    exp_top, exp_bottom = fst5, L + fst3
    problems = []
    if rsite != site: problems.append(f"site {site} != {rsite}")
    if (top, bottom) != (exp_top, exp_bottom): problems.append(f"cuts {top},{bottom} != {exp_top},{exp_bottom}")
    if scd5 is None and top2:
        problems.append("second cut given, but REBASE has one cut")
    elif scd5 is not None and (not top2 or (int(top2), int(bottom2)) != (scd5, L + scd3)):
        problems.append(f"second cut {top2},{bottom2} != {scd5},{L + scd3}")
    if problems:
        print(f"{name}: " + "; ".join(problems)); bad += 1
print(f"checked {len(ours)} enzymes, {bad} problem(s)")
sys.exit(1 if bad else 0)
