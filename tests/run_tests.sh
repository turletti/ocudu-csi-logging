#!/bin/bash
# Tests of the per-RB SRS CSI logger for OCUDU.
# Usage: run_tests.sh <patched_ocudu_source_dir> <cmake_build_dir>
#   The build dir must contain the libraries of the SRS estimator benchmark:
#   ninja -C <cmake_build_dir> srs_estimator_benchmark
set -euo pipefail
cd "$(dirname "$0")"
[ $# -eq 2 ] || { echo "usage: $0 <patched_ocudu_source_dir> <cmake_build_dir>" >&2; exit 2; }
SRC="$(cd "$1" && pwd)"; BUILD="$(cd "$2" && pwd)"
L=../src
OCUDU_INC="-I$SRC/include -I$SRC/external -I$SRC/external/fmt/include"
OCUDU_LIBS="-Wl,--start-group $(find "$BUILD/lib" "$BUILD/external" -name '*.a' | tr '\n' ' ') -Wl,--end-group -lfftw3f"
T=$(mktemp -d)

# 1. RB mapping with OCUDU's own get_srs_information() (1080 SRS configurations)
g++ -std=gnu++17 -O1 -g -I$L $OCUDU_INC test_mapping.cpp $L/srs_csi_rb_logger.cpp $OCUDU_LIBS -lpthread -o $T/test_mapping
$T/test_mapping

# 2. Logger alone: 4 threads, format, values, under ASan/UBSan and TSan
for san in address,undefined thread; do
  exe=$T/test_logger_${san%%,*}
  g++ -std=c++17 -O1 -g -fsanitize=$san -I$L test_logger.cpp $L/srs_csi_rb_logger.cpp -lpthread -o $exe
  d=$(mktemp -d -p $T)
  CSI_ENABLED=true CSI_OUTPUT_DIR=$d $exe 3 132 250 20000 > $d/log 2>&1
  ! grep -q "Sanitizer\|runtime error" $d/log || { echo "SANITIZER ($san)"; grep -m5 "#0\|SUMMARY" $d/log; exit 1; }
  python3 check_csv_v31.py $d/csi_per_rb.csv --expect-rows $((4 * 250 * 2 * 132))
  python3 - $d/csi_per_rb.csv <<'PY'
import sys, json
L = open(sys.argv[1]).read().splitlines(); m = json.loads(next(l for l in L if l.startswith('# {'))[2:])
assert m["source"] == "ocudu-srs" and m["iq_format"] == "float" and m["nb_antenna_rx"] == 2, m
bad = 0
for l in L:
    if not l[:1].isdigit(): continue
    f, s, r, a, p, rb, re_, im = l.split(","); t = int(r, 16) - 0x4600
    if float(re_) != int(rb) + 0.25 or float(im) != -(1000 * t + 10 * int(a) + int(p)) - 0.5 or int(s) != 2 * t: bad += 1
print("  values: wrong", bad); assert bad == 0
PY
done

# 2b. Multi-port SRS: leakage of the other ports into the per-RB value, plain mean vs N-pilot groups (ASan/UBSan)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I$L test_port_leakage.cpp $L/srs_csi_rb_logger.cpp -lpthread \
  -o $T/test_port_leakage
$T/test_port_leakage

# 3. Overload: ring of 1e6 rows full between two flushes -> rows dropped and counted
d=$(mktemp -d -p $T); CSI_ENABLED=1 CSI_OUTPUT_DIR=$d $T/test_logger_address 0 132 2000 0 > /dev/null 2>&1
python3 check_csv_v31.py $d/csi_per_rb.csv --expect-rows $((4 * 2000 * 2 * 132))

# 4. Selections: antenna 1 only; disabled logger creates no file
d=$(mktemp -d -p $T); CSI_ENABLED=true CSI_OUTPUT_DIR=$d CSI_ANTENNA_SELECTION=1 $T/test_logger_address 0 10 5 0 > /dev/null 2>&1
ants=$(grep '^[0-9]' $d/csi_per_rb.csv | cut -d, -f4 | sort -u | tr '\n' ' ')
echo "  antenna selection 1: ants in CSV = $ants"; [ "$ants" = "1 " ] || { echo "antenna selection"; exit 1; }
d=$(mktemp -d -p $T); CSI_OUTPUT_DIR=$d $T/test_logger_address 0 10 5 0
[ ! -e $d/csi_per_rb.csv ] || { echo "file created while disabled"; exit 1; }

# 5. End to end in OCUDU's SRS estimator: known channel -> logged values, RB/sc indices, ports, RNTI, TA, header,
#    in rb and subcarrier granularity (sampling 1, 2, 4); 1, 2 and 4 SRS ports, comb 2 and 4, interleaved combs
g++ -std=gnu++17 -O2 $OCUDU_INC test_ocudu_srs_e2e.cpp $OCUDU_LIBS -lpthread -o $T/test_ocudu_srs_e2e
for mode in rb:1 subcarrier:1 subcarrier:2 subcarrier:4; do
  d=$(mktemp -d -p $T)
  CSI_ENABLED=1 CSI_OUTPUT_DIR=$d CSI_GRANULARITY=${mode%%:*} CSI_SUBCARRIER_SAMPLING=${mode##*:} \
    $T/test_ocudu_srs_e2e $d > $d/log 2>&1 || { cat $d/log; exit 1; }
  python3 check_csv_v31.py $d/csi_per_rb.csv
  python3 check_ocudu_e2e.py $d
done

rm -rf $T
echo "ALL OK"
