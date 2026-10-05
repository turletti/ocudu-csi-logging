#!/usr/bin/env python3
"""Conformance check of a CSI CSV file (format v3.1, OAI or OCUDU/srsRAN producer).
   check_csv_v31.py FILE [--expect-rows N] : prints a summary, exits 1 on a format error."""
import json, re, sys
path = sys.argv[1]
exp = int(sys.argv[sys.argv.index('--expect-rows') + 1]) if '--expect-rows' in sys.argv else None
L = open(path).read().splitlines()
js = [json.loads(l[2:]) for l in L if l.startswith('# {')]
hdr = [l for l in L if l.startswith('frame')]
ts = [l for l in L if l.startswith('# TIMESTAMP:')]
dropped = sum(int(l.split(':')[1]) for l in L if l.startswith('# DROPPED:'))
rows = [l for l in L if l[:1].isdigit()]
err = []
if len(js) != 1: err.append(f"{len(js)} JSON headers (1 expected)")
if len(hdr) != 1: err.append(f"{len(hdr)} column lines (1 expected)")
m = js[0] if js else {}
for k in ("format_version", "columns", "granularity", "timestamp", "flush_period_s"):
    if k not in m: err.append(f"JSON field {k} missing")
if m.get("format_version") != "3.1": err.append("format_version != 3.1")
cols = m.get("columns", [])
if hdr and hdr[0].split(",") != cols: err.append("column line != JSON columns")
if not ts: err.append("no TIMESTAMP marker")
for t in ts:
    if not re.fullmatch(r"# TIMESTAMP: \d{4}-\d\d-\d\d \d\d:\d\d:\d\d", t): err.append(f"bad marker {t}"); break
# every data row after a marker, with the declared number of columns and parseable values
first_marker = next((i for i, l in enumerate(L) if l.startswith('# TIMESTAMP:')), len(L))
if any(l[:1].isdigit() for l in L[:first_marker]): err.append("data rows before the first marker")
for r in rows[:200000]:
    f = r.split(",")
    if len(f) != len(cols): err.append(f"row with {len(f)} fields: {r}"); break
    try:
        int(f[0]); int(f[1]); int(f[2], 16); [int(x) for x in f[3:-2]]; float(f[-2]); float(f[-1])
    except ValueError:
        err.append(f"unparseable row {r}"); break
if exp is not None and len(rows) + dropped != exp: err.append(f"rows {len(rows)} + dropped {dropped} != expected {exp}")
print(f"{path}: source={m.get('source', 'oai-srs (implicit)')} iq={m.get('iq_format', 'int16')} rows={len(rows)} "
      f"dropped={dropped} markers={len(ts)} -> {'OK' if not err else 'ERRORS: ' + '; '.join(err)}")
sys.exit(1 if err else 0)
