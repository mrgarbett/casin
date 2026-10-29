// SPDX-License-Identifier: GPL-3.0-or-later
// The pieces the Add Primer wizard can insert into a primer: codons, tags and
// protease sites. Enzyme sites come from the document's enzyme list.
import Foundation

enum PrimerParts {
    struct AminoAcid {
        let name: String     // "Leu (L)"
        let codons: [String] // the one E. coli uses most first
    }

    /// The standard genetic code, by amino acid, stops last.
    static let aminoAcids: [AminoAcid] = {
        let bases = Array("TCAG")
        let code = Array("FFLLSSSSYY**CC*WLLLLPPPPHHQQRRRRIIIMTTTTNNKKSSRRVVVVAAAADDEEGGGG")
        var codons: [Character: [String]] = [:]
        for (i, aa) in code.enumerated() {
            codons[aa, default: []].append(String([bases[i / 16], bases[i / 4 % 4], bases[i % 4]]))
        }
        // The codon E. coli uses most for each amino acid.
        let preferred: [Character: String] = [
            "A": "GCG", "R": "CGC", "N": "AAC", "D": "GAT", "C": "TGC", "Q": "CAG", "E": "GAA", "G": "GGC",
            "H": "CAT", "I": "ATT", "L": "CTG", "K": "AAA", "M": "ATG", "F": "TTT", "P": "CCG", "S": "AGC",
            "T": "ACC", "W": "TGG", "Y": "TAT", "V": "GTG", "*": "TAA",
        ]
        let names: [(Character, String)] = [
            ("A", "Ala"), ("R", "Arg"), ("N", "Asn"), ("D", "Asp"), ("C", "Cys"), ("Q", "Gln"), ("E", "Glu"),
            ("G", "Gly"), ("H", "His"), ("I", "Ile"), ("L", "Leu"), ("K", "Lys"), ("M", "Met"), ("F", "Phe"),
            ("P", "Pro"), ("S", "Ser"), ("T", "Thr"), ("W", "Trp"), ("Y", "Tyr"), ("V", "Val"),
        ]
        func ordered(_ aa: Character) -> [String] {
            let all = codons[aa] ?? []
            guard let best = preferred[aa], all.contains(best) else { return all }
            return [best] + all.filter { $0 != best }
        }
        return names.map { AminoAcid(name: "\($0.1) (\($0.0))", codons: ordered($0.0)) }
            + [AminoAcid(name: "Stop", codons: ordered("*"))]
    }()

    struct Tag {
        let name: String
        let protein: String  // what it encodes; "/" marks where a protease cuts
        let bases: String
    }

    /// Common tags and protease sites, as E. coli-friendly codons.
    static let tags: [Tag] = [
        Tag(name: "His6 tag", protein: "HHHHHH", bases: "CATCATCACCACCATCAC"),
        Tag(name: "FLAG tag", protein: "DYKDDDDK", bases: "GATTACAAGGATGACGACGATAAG"),
        Tag(name: "HA tag", protein: "YPYDVPDYA", bases: "TACCCATACGATGTTCCAGATTACGCT"),
        Tag(name: "Myc tag", protein: "EQKLISEEDL", bases: "GAACAAAAACTCATCTCAGAAGAGGATCTG"),
        Tag(name: "Strep-tag II", protein: "WSHPQFEK", bases: "TGGAGCCACCCGCAGTTCGAAAAA"),
        Tag(name: "TEV site", protein: "ENLYFQ/G", bases: "GAAAACCTGTATTTTCAGGGC"),
        Tag(name: "HRV 3C (PreScission) site", protein: "LEVLFQ/GP", bases: "CTGGAAGTTCTGTTCCAGGGGCCC"),
        Tag(name: "Thrombin site", protein: "LVPR/GS", bases: "CTGGTGCCGCGTGGCAGC"),
        Tag(name: "Factor Xa site", protein: "IEGR/", bases: "ATCGAAGGTCGT"),
        Tag(name: "Enterokinase site", protein: "DDDDK/", bases: "GATGACGATGACAAG"),
    ]

    /// The reverse complement, keeping case and IUPAC codes.
    static func reverseComplement(_ s: String) -> String {
        let pairs: [Character: Character] = [
            "A": "T", "T": "A", "G": "C", "C": "G", "R": "Y", "Y": "R", "K": "M", "M": "K",
            "B": "V", "V": "B", "D": "H", "H": "D", "S": "S", "W": "W", "N": "N",
        ]
        return String(s.reversed().map { c -> Character in
            let up = Character(c.uppercased())
            guard let p = pairs[up] else { return c }
            return c.isLowercase ? Character(p.lowercased()) : p
        })
    }
}
