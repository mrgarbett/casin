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

## Gateway att sites

Recombination crosses inside the core every att site of one number shares, so
`src/core/gateway.cpp` holds one 15 bp core per number rather than whole att
sites. Every core is written in the same 5'->3' sense, so they all share their first
six bases. The two sites flanking an insert face each other, so one is found
on the top strand and the other reversed. Which one is reversed depends on how
the molecule was written, not on the number: in the plasmids checked below,
att2 reads on the bottom strand where att1 reads on the top.

| Number | Core | Where it comes from |
| --- | --- | --- |
| att1 | `TTTGTACAAAAAAGC` | the middle of attB1 (25 bp), Invitrogen Gateway Technology manual |
| att2 | `TTTGTACAAGAAAGC` | the middle of attB2 (25 bp), same manual |
| att3 | `TTTGTATAATAAAGT` | the middle of the attB3 primer tail, MultiSite Gateway manual |
| att4 | `TTTGTATAGAAAAGT` | the middle of the attB4 primer tail, same manual |

att5 is not included yet, so three-fragment MultiSite (att4-att1, att1-att2,
att2-att3) works and four-fragment doesn't.

An earlier version stored att2 the other way round, which made both sites of
every real Gateway plasmid read on the same strand, so every reaction was
refused (reported against PR #38 on a pDONR201 entry clone and pB7FWG2, which
now give the expected 10,788 bp expression clone). `testGateway` now checks
that all four cores share their first bases, which is what went wrong. att3
and att4 follow the same sense as att1, but no real MultiSite file has been
tested against them yet.

The manuals state that the att sites of one number share a 15 bp core and that
the strands cross within it; the products of a reaction therefore carry half
the core from either parent, which `testGateway` checks by rebuilding each
core whole across both joins.

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
