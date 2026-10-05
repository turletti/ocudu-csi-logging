#!/bin/bash
# Adds the per-RB SRS CSI logger (CSI CSV format v3.1) to an OCUDU source tree, by text anchors.
#
# Usage: patch_ocudu_srs_csi.sh [--check] <ocudu_source_dir>
#   --check  only verify that every anchor and identifier the patch relies on is present (no modification)
#
# Every step checks that its anchor occurs exactly once and fails with an explicit message otherwise, so that an
# upstream change is detected here and not as a compilation error. Already applied steps are skipped (idempotent).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$HERE/src"

CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
  CHECK_ONLY=1
  shift
fi
[ $# -eq 1 ] || { echo "usage: $0 [--check] <ocudu_source_dir>" >&2; exit 2; }
OCUDU="$(cd "$1" && pwd)"

SRS_DIR="$OCUDU/lib/phy/upper/signal_processors/srs"
CMAKE="$OCUDU/lib/phy/upper/signal_processors/CMakeLists.txt"
CTX="$OCUDU/include/ocudu/ran/srs/srs_context.h"
EST="$SRS_DIR/srs_estimator_generic_impl.cpp"
CFG="$OCUDU/include/ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"

# Anchors (fixed strings).
A_CMAKE_LIB='add_library(ocudu_srs_estimator STATIC'
A_CMAKE_SRC='srs/srs_estimator_generic_impl.cpp'
A_CTX='explicit srs_context('
A_CTX_RNTI='rnti_t   rnti'
A_CFG_CTX='std::optional<srs_context> context;'
A_EST_INC='#include "srs_estimator_generic_impl.h"'
A_EST_HOOK='compensate_phase_shift(mean_lse, phase_shift_subcarrier, phase_shift_offset);'
# Identifiers used by the hook, in the scope of the anchor.
EST_IDENTIFIERS=(nof_tx_antenna_ports nof_rx_ports comb_size 'config.ports[' 'config.slot' 'info.mapping_initial_subcarrier' 'i_rx_port' 'i_antenna_port' 'span<cf_t> mean_lse')

die() { echo "patch_ocudu_srs_csi: ERROR: $*" >&2; exit 1; }
count() { grep -cF -- "$2" "$1" || true; }
expect_once() { # file anchor
  [ -f "$1" ] || die "file not found: ${1#$OCUDU/}"
  local n; n=$(count "$1" "$2")
  [ "$n" = 1 ] || die "anchor '$2' found $n times in ${1#$OCUDU/} (1 expected): upstream changed, update the patch"
}

# ---- 0. Check all anchors and identifiers before touching anything.
for f in srs_csi_rb_logger.h srs_csi_rb_logger.cpp srs_estimator_hook.inc; do
  [ -f "$SRC_DIR/$f" ] || die "missing $SRC_DIR/$f"
done
expect_once "$CMAKE" "$A_CMAKE_LIB"
expect_once "$CMAKE" "$A_CMAKE_SRC"
grep -A6 -F -- "$A_CMAKE_LIB" "$CMAKE" | grep -qF -- "$A_CMAKE_SRC" ||
  die "$A_CMAKE_SRC is not in the source list of ocudu_srs_estimator"
expect_once "$CTX" "$A_CTX"
expect_once "$CTX" "$A_CTX_RNTI"
expect_once "$CFG" "$A_CFG_CTX"
expect_once "$EST" "$A_EST_INC"
expect_once "$EST" "$A_EST_HOOK"
for id in "${EST_IDENTIFIERS[@]}"; do
  grep -qF -- "$id" "$EST" || die "identifier '$id' not found in ${EST#$OCUDU/}: upstream changed, update the hook"
done
if [ $CHECK_ONLY = 1 ]; then
  echo "patch_ocudu_srs_csi: all anchors found in $OCUDU"
  exit 0
fi

# insert_after FILE ANCHOR CONTENT_FILE: inserts CONTENT_FILE after the (single) line containing ANCHOR.
insert_after() {
  awk -v a="$2" -v f="$3" '{ print } index($0, a) { while ((getline l < f) > 0) print l; close(f) }' "$1" > "$1.tmp"
  mv "$1.tmp" "$1"
}

# ---- 1. Logger sources.
cp "$SRC_DIR/srs_csi_rb_logger.h" "$SRC_DIR/srs_csi_rb_logger.cpp" "$SRS_DIR/"
echo "  [1] logger copied to ${SRS_DIR#$OCUDU/}"

# ---- 2. CMake: add the logger to the ocudu_srs_estimator sources (same indentation as the anchor line).
if grep -qF 'srs/srs_csi_rb_logger.cpp' "$CMAKE"; then
  echo "  [2] CMake: already done"
else
  awk -v a="$A_CMAKE_SRC" 'index($0, a) { match($0, /^[ \t]*/); print substr($0, 1, RLENGTH) "srs/srs_csi_rb_logger.cpp" }
                           { print }' "$CMAKE" > "$CMAKE.tmp"
  mv "$CMAKE.tmp" "$CMAKE"
  echo "  [2] CMake: srs/srs_csi_rb_logger.cpp added to ocudu_srs_estimator"
fi

# ---- 3. srs_context: public RNTI getter (the RNTI is private, only read by its fmt formatter).
if grep -qF 'get_rnti()' "$CTX"; then
  echo "  [3] srs_context: get_rnti() already present"
else
  tmp=$(mktemp)
  cat > "$tmp" <<'EOF'

  /// Returns the RNTI of the UE that transmitted the SRS.
  rnti_t get_rnti() const { return rnti; }
EOF
  insert_after "$CTX" "$A_CTX" "$tmp"
  rm -f "$tmp"
  echo "  [3] srs_context: get_rnti() added"
fi

# ---- 4. SRS estimator: include and hook after the TA compensation of mean_lse.
if grep -qF 'csi_log::srs_csi_rb_logger::get()' "$EST"; then
  echo "  [4] SRS estimator: already done"
else
  tmp=$(mktemp)
  printf '#include "srs_csi_rb_logger.h"\n' > "$tmp"
  insert_after "$EST" "$A_EST_INC" "$tmp"
  rm -f "$tmp"
  insert_after "$EST" "$A_EST_HOOK" "$SRC_DIR/srs_estimator_hook.inc"
  echo "  [4] SRS estimator: include and hook added"
fi

# ---- 5. Verify the result.
[ "$(count "$CMAKE" 'srs/srs_csi_rb_logger.cpp')" = 1 ] || die "verification: CMake"
[ "$(count "$CTX" 'get_rnti()')" = 1 ] || die "verification: srs_context getter"
[ "$(count "$EST" '#include "srs_csi_rb_logger.h"')" = 1 ] || die "verification: estimator include"
[ "$(count "$EST" 'csi_log::srs_csi_rb_logger::get()')" = 1 ] || die "verification: estimator hook"
# The hook must directly follow the mean_lse compensation (and not the noise_help one).
grep -A2 -F -- "$A_EST_HOOK" "$EST" | grep -qF 'CSI logging (CSI CSV format v3.1' || die "verification: hook position"
cmp -s "$SRC_DIR/srs_csi_rb_logger.cpp" "$SRS_DIR/srs_csi_rb_logger.cpp" || die "verification: logger copy"
echo "patch_ocudu_srs_csi: OK ($OCUDU)"
