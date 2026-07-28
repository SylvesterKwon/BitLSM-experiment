#!/usr/bin/env python3
"""original/*.sql (Vertica/Tableau dialect) -> queries/*.sql (MySQL 8.0).

Deterministic, auditable rewrite (README mirrors this rule list):
 R1 all "Taxpayer_N" refs -> canonical bare table `taxpayer`; column
    qualifiers stripped (single-table queries; bare FROM keeps the harness
    hint-injection regex working — the first occurrence of the table name
    in the SQL must be the FROM reference).
 R2 double-quoted identifiers: known column names -> bare; anything else
    (Tableau aliases containing ':'/'(') -> backticks.
 R3 CAST(x AS double) -> x (operands are already DOUBLE; the cast is a
    no-op and MySQL's spelling differs — semantics identical).
 R4 CAST(x AS BIGINT) -> CAST(x AS SIGNED).
Whitespace collapsed; everything else (constants, predicates, GROUP BY
shape, duplicate GROUP BY terms in q20 — legal in MySQL) is verbatim.
"""
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "src"))
from myrocks.workloads.pbi_taxpayer import COLUMNS  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
COLSET = {c for c, _, _ in COLUMNS}


def strip_double_casts(sql: str) -> str:
    out, i = [], 0
    while True:
        j = sql.find("CAST(", i)
        if j < 0:
            out.append(sql[i:])
            return "".join(out)
        depth, k = 0, j + 4
        while True:
            if sql[k] == "(":
                depth += 1
            elif sql[k] == ")":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        inner = sql[j + 5:k]
        if inner.rstrip().upper().endswith("AS DOUBLE"):
            expr = inner.rstrip()[:-len("AS DOUBLE")].rstrip()
            out.append(sql[i:j])
            out.append(expr)
        else:
            out.append(sql[i:k + 1])
        i = k + 1


def rewrite(sql: str) -> str:
    sql = re.sub(r'"Taxpayer_\d+"\.', "", sql)         # R1 qualifiers
    sql = re.sub(r'"Taxpayer_\d+"', "taxpayer", sql)   # R1 FROM ref

    def quoted(m):                                     # R2
        name = m.group(1)
        return name if name in COLSET else f"`{name}`"
    sql = re.sub(r'"([^"]+)"', quoted, sql)
    sql = strip_double_casts(sql)                      # R3
    sql = sql.replace(" AS BIGINT)", " AS SIGNED)")    # R4
    return re.sub(r"[ \t]+", " ", sql).strip()


def main():
    src = os.path.join(HERE, "original")
    dst = os.path.join(HERE, "queries")
    os.makedirs(dst, exist_ok=True)
    for fname in sorted(os.listdir(src)):
        m = re.fullmatch(r"q(\d+)\.sql", fname)
        if not m:
            continue
        with open(os.path.join(src, fname)) as f:
            out = rewrite(f.read())
        assert '"' not in out, fname
        assert "Taxpayer" not in out, fname
        assert "BIGINT" not in out, fname
        assert not re.search(r"AS\s+double", out, re.I), fname
        with open(os.path.join(dst, f"q{int(m.group(1)):02d}.sql"),
                  "w") as f:
            f.write(out + "\n")
        print(f"q{int(m.group(1)):02d} ok")


if __name__ == "__main__":
    main()
