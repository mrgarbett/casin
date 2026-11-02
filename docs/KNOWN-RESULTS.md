# Known results

Tests in `tests/test_core.cpp` (`testReferences`) check the core against
published numbers on public sequences. Each expected value is listed here with
its source, so a failing test points to a real disagreement with the
literature, not to a number we made up.

## Reference sequences

| File | Record | Notes |
| --- | --- | --- |
| `samples/reference/pUC19-L09137.gb` | GenBank L09137.2, pUC19 cloning vector, 2,686 bp, circular | Yanisch-Perron, Vieira and Messing (1985) *Gene* 33:103–119 |
| `samples/reference/lambda-NC_001416.gb` | RefSeq NC_001416.1, Enterobacteria phage lambda, 48,502 bp, linear | Sanger et al. (1982) *J Mol Biol* 162:729–773 |

Both are unmodified NCBI flat files (NCBI sequence records are public data).
They were copied from the test data of the pydna project, because this build
environment can't reach NCBI directly; the LOCUS, ACCESSION and VERSION lines
and the lengths match the records above.

## Expected results

| Check | Expected | Source |
| --- | --- | --- |
| pUC19 length and topology | 2,686 bp, circular | GenBank L09137.2 |
| pUC19 EcoRI site | one, at 396 | NEB pUC19 restriction map |
| pUC19 HindIII site | one, at 447 | NEB pUC19 restriction map |
| pUC19 PvuII digest | 2,364 + 322 bp | NEB pUC19 restriction map |
| Lambda length and topology | 48,502 bp, linear | RefSeq NC_001416.1 |
| Lambda EcoRI digest | 21,226; 7,421; 5,804; 5,643; 4,878; 3,530 bp | Lambda DNA/EcoRI marker band sizes (NEB, Thermo Fisher) |
| Lambda HindIII digest, wild-type record | 23,130; 9,416; 6,682; 4,361; 2,322; 2,027; 564 bp | Computed from NC_001416.1, six AAGCTT sites |
| Lambda HindIII digest, marker DNA | 23,130; 9,416; 6,557; 4,361; 2,322; 2,027; 564; 125 bp | NEB Lambda DNA-HindIII Digest (N3012) |

The two HindIII rows differ on purpose. Commercial lambda markers are made from
the cI857ind1 Sam7 strain. That DNA has a seventh HindIII site at 37,584, which
the wild-type record lacks (it reads AAGCTC there). The test makes that one-base
change and then expects NEB's eight bands.

## Primers and PCR (pUC19)

| Check | Expected | Source |
| --- | --- | --- |
| Tm, M13/pUC forward (-20) `GTAAAACGACGGCCAGT` | 52.77 °C (within 0.1) | Primer3 `calc_tm`, SantaLucia 1998, 50 mM Na+, 500 nM oligo |
| Tm, M13/pUC reverse `CAGGAAACAGCTATGAC` | 47.18 °C | same |
| Tm, four more primers incl. a self-complementary one | see `testPrimersOnReference` | same |
| M13 forward binding site | 379–395, top strand, exact | L09137.2 sequence |
| M13 reverse binding site | 465–481, bottom strand, exact | L09137.2 sequence |
| PCR with M13 forward + reverse | 103 bp (379–481), spans the MCS | L09137.2 sequence |
| Same PCR with a 7 nt EcoRI and a 9 nt HindIII 5′ tail | 119 bp, tails at the ends | 103 + 7 + 9 |
| Primer design for the MCS (396–452), 50 bp flank, Tm 60 | both primers within the flank, Tm 60 ± 3 °C, within 2 °C of each other | design target |

Tm agreement with Primer3 is exact to two decimals for every primer tested, because both use the same SantaLucia (1998) unified parameters and salt correction.
