// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace oc {

// IUPAC nucleotide code as a bitmask: A=1, C=2, G=4, T=8. 0 = not a base.
// Lowercase is accepted. U is treated as T.
inline unsigned iupacMask(char c)
{
    switch (c) {
    case 'A': case 'a': return 1;
    case 'C': case 'c': return 2;
    case 'G': case 'g': return 4;
    case 'T': case 't': case 'U': case 'u': return 8;
    case 'R': case 'r': return 1 | 4;
    case 'Y': case 'y': return 2 | 8;
    case 'S': case 's': return 2 | 4;
    case 'W': case 'w': return 1 | 8;
    case 'K': case 'k': return 4 | 8;
    case 'M': case 'm': return 1 | 2;
    case 'B': case 'b': return 2 | 4 | 8;
    case 'D': case 'd': return 1 | 4 | 8;
    case 'H': case 'h': return 1 | 2 | 8;
    case 'V': case 'v': return 1 | 2 | 4;
    case 'N': case 'n': return 15;
    default: return 0;
    }
}

// True when a pattern code matches a sequence base: every base the sequence
// letter could be must be allowed by the pattern (so N in the sequence only
// matches N in the pattern).
inline bool iupacMatches(unsigned patternMask, unsigned baseMask)
{
    return baseMask && (baseMask & ~patternMask) == 0;
}

} // namespace oc
