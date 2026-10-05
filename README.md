# Per-RB SRS CSI logger for OCUDU (CSI CSV format v3.1)

Adds to OCUDU (ex srsRAN Project) a per-RB SRS channel logger that writes the CSI CSV format v3.1 of
[oai-csi-logging](https://github.com/turletti/oai-csi-logging), so that OCUDU and OAI runs share the same
collection (5g_ansible), live view and analysis tools. No fork: the OCUDU tree is patched by text anchors.

## Contents

- `src/srs_csi_rb_logger.{h,cpp}`: the logger, standard C++ only, namespace `csi_log`.
- `src/srs_estimator_hook.inc`: the code inserted in `srs_estimator_generic_impl.cpp`, after the TA compensation of
  `mean_lse`.
- `patch_ocudu_srs_csi.sh [--check] <ocudu_dir>`: applies the patch (idempotent). Every anchor must occur once,
  otherwise it stops with an explicit message. `--check` only verifies the anchors. Apply it to a clean tree: an
  already patched tree is left as is (an older hook is not updated).
- `build_image.sh`: clones an OCUDU tag, applies the script and builds the gNB image (see below).
- `docker/Dockerfile.zmq`: ZeroMQ variant (the upstream images are built without ZeroMQ).
- `ocudu-release_26_10-srs-csi.diff`: the resulting diff on `release_26_10`, for review only (use the script).
- `tests/`: see below.

Changes made in the OCUDU tree:

1. `lib/phy/upper/signal_processors/srs/srs_csi_rb_logger.{h,cpp}` added, and listed in `ocudu_srs_estimator`
   (`lib/phy/upper/signal_processors/CMakeLists.txt`).
2. `include/ocudu/ran/srs/srs_context.h`: public getter `get_rnti()` (the RNTI was private).
3. `srs_estimator_generic_impl.cpp`: include and hook (`log_pilots()`). The RNTI comes from `config.context` (filled from the FAPI
   SRS PDU), 0 when absent. `ant_rx` is the physical RX port `config.ports[i]`.

## Run time

- `CSI_ENABLED=true|1` (otherwise nothing is created and the hook costs one pointer test).
- `CSI_OUTPUT_DIR` (default `/data/csi`, file `csi_per_rb.csv`, truncated at gNB start). The runtime images of
  OCUDU run as uid 1001: the hostPath must be writable by that uid.
- `CSI_FLUSH_CORE`: housekeeping CPU for the writer thread (recommended).
- `CSI_ANTENNA_SELECTION`, `CSI_PORT_SELECTION` (`all` or comma list), `CSI_INCLUDE_HEADER`. Antennas are the
  physical RX ports; the JSON `antenna_selection` / `port_selection` list the values present in the file.
- `CSI_GRANULARITY`: `rb` (default, complex mean of the SRS pilots of each RB) or `subcarrier` (one row per SRS
  pilot, column `sc` = subcarrier in the RB). Unlike OAI, which interpolates its SRS estimate on the 12 subcarriers,
  OCUDU only has the LS estimate on the pilots (every comb-th subcarrier): `sc` takes the values k_TC + j·comb, and
  the JSON says so (`sc_values`, `srs_comb` of the first occasion). No interpolation.
- `CSI_SUBCARRIER_SAMPLING` (1..12, `subcarrier` only): keeps 1 pilot out of N in each RB, i.e. the pilots whose
  index in the RB (`sc / comb`) is a multiple of N. The OAI rule (`sc % N == 0`) is not used: with an odd comb
  offset it can keep no pilot at all. A `subcarrier` file has up to 6x the rows of an `rb` file (comb 2): the
  1e6-row buffer per 5 s fills faster (rows dropped and counted).
- Periodic SRS must be configured in the gNB. Only single-port SRS is logged.

The logger is created before `main()` (static initialisation), so the preallocation of the two 1e6-row buffers
(~20 ms) does not happen in a PHY thread on the first SRS occasion.

## Image

```bash
./build_image.sh -t release_26_10 -m <march of the RAN node> -v dpdk    # ocudu-gnb-csi:release_26_10
./build_image.sh -t release_26_10 -m <march of the RAN node> -v zmq     # ocudu-gnb-csi:release_26_10-zmq
./build_image.sh -s <ocudu source tree> ...                               # no clone (e.g. extracted git archive)
```

- `dpdk`: upstream `docker/Dockerfile`, unmodified, target `runtime-dpdk`, `COMPONENT=gnb`. It runs as uid 1001
  (file capabilities on `gnb`).
- `zmq`: `docker/Dockerfile.zmq`, which reuses the upstream build scripts and checks that `gnb` links libzmq.
- Labels: `ocudu.tag`, `ocudu.revision`, `csi.logger.revision`, `csi.format_version`.
- `-m` defaults to `native`, as upstream: only right when the build host has the CPU family of the RAN node.

## Tests

```bash
ninja -C <build_dir> srs_estimator_benchmark      # builds the libraries the tests link with
tests/run_tests.sh <patched_ocudu_dir> <build_dir>
```

1. `test_mapping`: RB index of the SRS pilots with OCUDU's `get_srs_information()`, 1080 SRS configurations.
2. `test_logger`: 4 threads, under ASan/UBSan and TSan; values encode their coordinates.
3. Overload: buffer full between two flushes, rows dropped and counted.
4. Antenna selection; disabled logger creates no file.
5. `test_ocudu_srs_e2e` + `check_ocudu_e2e.py`: a known channel is written on the SRS pilots of a resource grid,
   OCUDU's SRS estimator runs, and the logged rows are compared with the expected ones. It checks the RB indices,
   the physical RX ports (grid ports 1 and 3), the RNTI from `srs_context` (and 0 without context), the frame and slot,
   the values (comb 2 and 4, 1 and 4 symbols, frequency shift) and the TA compensation (0.3 us delay), and that
   2-port SRS is not logged; in `rb` granularity and in `subcarrier` granularity with sampling 1, 2 and 4 (JSON
   header and column lists checked too).

`check_csv_v31.py FILE` also accepts OAI files. `test_latency.cpp` is informative only.
