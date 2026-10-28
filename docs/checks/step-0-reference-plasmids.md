# Manual check: step 0, reference plasmids

Use the app built from main, on the Mac first, then on Fedora. Write down
anything that doesn't match.

## pUC19

1. Open `samples/reference/pUC19-L09137.gb`.
   - [ ] The title reads 2,686 bp, circular.
2. In the Enzymes list (on the Mac, the Enzymes tab of the inspector on the right), set the filter to **Up to 2 cuts**.
   - [ ] EcoRI is listed at 396 and HindIII at 447.
3. Cloning → Digest and Gel, choose **EcoRI**.
   - [ ] One fragment, 2,686 bp.
4. Digest and Gel again, choose **PvuII**.
   - [ ] Two fragments: 2,364 bp and 322 bp. On the gel the 322 bp band runs far below the 2,364 bp one.

## Lambda

5. Open `samples/reference/lambda-NC_001416.gb`.
   - [ ] The title reads 48,502 bp, linear.
6. Set the enzyme filter to **All cutters**, then Digest and Gel with **EcoRI**.
   - [ ] Six fragments: 21,226, 7,421, 5,804, 5,643, 4,878 and 3,530 bp.
7. Digest and Gel with **HindIII**.
   - [ ] Seven fragments: 23,130, 9,416, 6,682, 4,361, 2,322, 2,027 and 564 bp.
   - This is the wild-type sequence, so there's no 125 bp band, and you'll see 6,682 where a bought marker shows 6,557 + 125 (see `docs/KNOWN-RESULTS.md`).

## Experimental Tools

8. With Experimental Tools off, the menus show only the core tools.
   - [ ] On the Mac there's no Align menu, Print or Golden Gate; on Fedora there's no Align and Compare, Print or Golden Gate.
