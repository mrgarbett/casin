# Restriction enzyme data

OpenClone's enzyme table (`src/core/enzymes.cpp`) comes from **REBASE**, the
Restriction Enzyme Database:

> Roberts, R.J., Vincze, T., Posfai, J., Macelis, D. REBASE in 2023: a database for
> enzymes, genes and methylation. *Nucleic Acids Res.* 51:D629–D630 (2023).
> https://rebase.neb.com

## Cut convention

Cuts are counted from the first base of the recognition site, in the site's own
orientation:

| Enzyme | REBASE notation | `cutTop`, `cutBottom` |
|--------|-----------------|-----------------------|
| EcoRI  | `G^AATTC`       | 1, 5                  |
| PstI   | `CTGCA^G`       | 5, 1                  |
| BsaI   | `GGTCTC(1/5)`   | 7, 11                 |

This matches the `c1`/`c2` columns of REBASE's EMBOSS export (`emboss_e.###`).

## Checking the table

`scripts/check_enzymes.py` compares every entry with REBASE's EMBOSS data as
packaged by Biopython (`Bio/Restriction/Restriction_Dictionary.py`, generated from
`emboss_e`). The last check used REBASE EMBOSS version 404 and found 94 enzymes with
0 mismatches:

    pip download biopython --no-deps && unzip biopython-*.whl 'Bio/Restriction/*'
    python3 scripts/check_enzymes.py Bio/Restriction/Restriction_Dictionary.py

Enzymes that cut twice per site (BaeI, CspCI, ...) aren't supported yet and aren't
in the table.
