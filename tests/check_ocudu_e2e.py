#!/usr/bin/env python3
"""Compares the CSI CSV written by test_ocudu_srs_e2e with its expected values.
   check_ocudu_e2e.py OUT_DIR : exits 1 on any mismatch.
   "rb" granularity: expected.csv; "subcarrier" granularity: expected_sc.csv, filtered with the sampling of the JSON
   header (keep the pilots whose index in the RB, sc // comb, is a multiple of N)."""
import cmath, csv, json, sys
from collections import Counter
d = sys.argv[1]
L = open(f"{d}/csi_per_rb.csv").read().splitlines()
meta = json.loads(next(l for l in L if l.startswith("# {"))[2:])
per_sc = meta.get("granularity") == "subcarrier"
N = int(meta.get("subcarrier_sampling", 1))
err = []

exp_cols = ["frame", "slot", "rnti", "ant_rx", "port_tx", "rb"] + (["sc"] if per_sc else []) + ["real", "imag"]
if meta.get("columns") != exp_cols:
    err.append(f"JSON columns {meta.get('columns')}")
hdr = next(l for l in L if l.startswith("frame"))
if hdr.split(",") != exp_cols:
    err.append(f"column line {hdr}")
if (meta.get("source") != "ocudu-srs" or meta.get("nb_antenna_rx") != 2 or meta.get("nb_ports_tx") != 1
        or meta.get("antenna_selection") != [1, 3] or meta.get("port_selection") != [0]):
    err.append(f"JSON header {meta}")
if per_sc and meta.get("srs_comb") != 2:
    err.append(f"JSON srs_comb {meta.get('srs_comb')} (2 expected: comb of the first occasion)")

def key(f):
    return tuple(int(x) if i != 2 else x for i, x in enumerate(f))

got = {}
for l in L:
    if l[:1].isdigit():
        f = l.split(",")
        got[key(f[:-2])] = complex(float(f[-2]), float(f[-1]))

exp, case_of_slot = {}, {}
rows = list(csv.DictReader(open(f"{d}/{'expected_sc.csv' if per_sc else 'expected.csv'}")))
if per_sc:  # comb of each case = 12 / pilots per (antenna, RB)
    per_rb = Counter((r["slot"], r["ant_rx"], r["rb"]) for r in rows)
    comb = {s: 12 // n for (s, _, _), n in per_rb.items()}
for r in rows:
    if per_sc and (int(r["sc"]) // comb[r["slot"]]) % N != 0:
        continue
    k = key([r[c] for c in exp_cols[:-2]])
    exp[k] = complex(float(r["real"]), float(r["imag"]))
    case_of_slot[int(r["slot"])] = r["case"]

if set(got) != set(exp):
    err.append(f"row keys differ: {len(set(got) - set(exp))} unexpected, {len(set(exp) - set(got))} missing; "
               f"e.g. unexpected {sorted(set(got) - set(exp))[:3]} missing {sorted(set(exp) - set(got))[:3]}")
for slot, case in sorted(case_of_slot.items()):
    keys = sorted(k for k in exp if k[1] == slot and k in got)
    rel = [abs(got[k] - exp[k]) / abs(exp[k]) for k in keys]
    mag = [abs(got[k]) / abs(exp[k]) - 1 for k in keys]
    dph = [cmath.phase(got[k] / exp[k]) for k in keys]
    scs = sorted({k[6] for k in keys}) if per_sc else None
    print(f"  case {case}: {len(keys)} rows, max rel err {max(rel):.4f}, max |mag| err {max(map(abs, mag)):.4f}, "
          f"residual phase [{min(dph):+.3f}, {max(dph):+.3f}] rad" + (f", sc {scs}" if per_sc else ""))
    if case == "C":  # delayed channel: TA compensated, so magnitude exact and phase nearly flat
        if max(map(abs, mag)) > 0.03 or max(dph) - min(dph) > 0.3:
            err.append("case C: TA compensation")
    elif max(rel) > 0.02:
        err.append(f"case {case}: values")
print(f"E2E ({meta.get('granularity')}, sampling {N})", "OK" if not err else "ERRORS: " + "; ".join(err))
sys.exit(1 if err else 0)
