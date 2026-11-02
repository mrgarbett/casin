# Manual check: steps 1 and 2, primer design and PCR

Use `samples/reference/pUC19-L09137.gb`. Mac first, then Fedora.

## Primer check

1. Check the primer `GTAAAACGACGGCCAGT` (M13 forward).
   - [ ] Tm reads 52.8 °C and length 17.
   - [ ] Two warnings and only these: short (17 nt), and no G or C at the 3′ end. Nothing about dimers or hairpins.
2. Check `CAGGAAACAGCTATGAC` (M13 reverse).
   - [ ] Tm reads 47.2 °C, with warnings for short (17 nt) and low Tm.

## Adding primers by sequence

3. Add both M13 primers to pUC19 by sequence.
   - [ ] M13 forward lands at 379–395 on the top strand.
   - [ ] M13 reverse lands at 465–481 on the bottom strand.

## PCR

4. Run PCR with the two M13 primers.
   - [ ] The product is 103 bp, linear, and contains the EcoRI to HindIII sites of the MCS.
5. Add `GGAATTCGTAAAACGACGGCCAGT` and `CCCAAGCTTCAGGAAACAGCTATGAC` (the same primers with EcoRI and HindIII tails) and run PCR with them.
   - [ ] The product is 119 bp and starts with GGAATTC.
6. Run PCR with the primers swapped (reverse as forward).
   - [ ] It refuses, saying the primers don't face each other.

## Primer design

7. Design primers for 396–452 (the MCS) with a 50 bp flank and Tm 60.
   - [ ] Both primers sit within 50 bp of the MCS, outside or on its ends.
   - [ ] Both Tms read between 57 and 63 °C, within 2 °C of each other.
   - [ ] PCR with the designed pair gives a product that contains the whole MCS.
