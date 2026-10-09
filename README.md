# SRS CSI logger for OCUDU (CSI CSV format v3.1)

Adds to OCUDU (ex srsRAN Project) an SRS channel logger (per RB or per pilot subcarrier) that writes the CSI CSV format v3.1 of
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
  OCUDU run as uid 1001: the hostPath must be writable by that uid. The directory must be on a disk: on a tmpfs
  (or an overlay on a tmpfs, e.g. a node booted live) the file is in RAM and charged to the gNB container memory,
  which ends in an OOM kill (a warning is printed at start). After each flush the writer calls `fdatasync` and
  `posix_fadvise(POSIX_FADV_DONTNEED)`, so the written pages leave the page cache, which is otherwise also charged
  to the container memory cgroup (a few MB/s with periodic SRS on many RBs).
- `CSI_FLUSH_CORE`: housekeeping CPU for the writer thread (recommended).
- `CSI_ANTENNA_SELECTION`, `CSI_PORT_SELECTION` (`all` or comma list), `CSI_INCLUDE_HEADER`. Antennas are the
  physical RX ports; the JSON `antenna_selection` lists the antennas present in the file. `nb_ports_tx` is the
  maximum number of SRS ports (4) and `port_selection` the selected ports among 0..3: the header is written at the
  first flush, usually before the UE capabilities raise the number of SRS ports. The `port_tx` column gives the port.
- `CSI_GRANULARITY`: `rb` (default, complex mean of the SRS pilots of each RB) or `subcarrier` (one row per SRS
  pilot, column `sc` = subcarrier in the RB). Unlike OAI, which interpolates its SRS estimate on the 12 subcarriers,
  OCUDU only has the LS estimate on the pilots (every comb-th subcarrier): `sc` takes the values k_TC + j·comb, and
  the JSON says so (`sc_values`, `srs_comb` of the first occasion). No interpolation.
- `CSI_SUBCARRIER_SAMPLING` (1..12, `subcarrier` only): keeps 1 pilot out of N in each RB, i.e. the pilots whose
  index in the RB (`sc / comb`) is a multiple of N. The OAI rule (`sc % N == 0`) is not used: with an odd comb
  offset it can keep no pilot at all. A `subcarrier` file has up to 6x the rows of an `rb` file (comb 2): the
  1e6-row buffer per 5 s fills faster (rows dropped and counted).
- Periodic SRS must be configured in the gNB.
- SRS ports. OCUDU takes the number of SRS ports from the UE capabilities for the band
  (`du_pusch_resource_manager::select_srs_nof_ports()`, no cell parameter caps it): 1 port until the UE capability
  exchange, then often 2 (or 4) after the RRC reconfiguration. With N ports on one comb, the cyclic shifts are
  n_cs_max/N apart, so the LS estimate of port p on pilot k is H_p[k] + Σ_{q≠p} H_q[k]·e^{j2π(q−p)k/N}: one pilot
  does not separate the ports. In `rb` granularity the value of a port is the mean of groups of N consecutive
  pilots (2 with 4 ports on interleaved combs), each group in the RB of its centre subcarrier: the other ports
  cancel when the channel is flat over the group. Simulated leakage of the other ports (3-tap channel up to about
  300 ns, 40 RB): comb 4 and 2 ports −35 dB (plain per-RB mean −9.5 dB), comb 2 and 4 ports −34 dB, comb 4 and 4
  ports on one comb −28 dB (`tests/test_port_leakage.cpp`). With comb 4 and 4 ports on one comb, a group covers
  16 subcarriers: RBs without a group centre are not logged. In `subcarrier` granularity multi-port occasions are
  not logged (one message on stderr). This assumes `cyclic_shift_reuse_factor: 1` (default): with cyclic shift
  reuse, other UEs use intermediate cyclic shifts that N-pilot groups do not cancel.

The logger is created before `main()` (static initialisation), so the preallocation of the two 1e6-row buffers
(~20 ms) does not happen in a PHY thread on the first SRS occasion.

## Image

```bash
./build_image.sh -t release_26_10 -m <march> -v dpdk    # ocudu-gnb-csi:release_26_10      O-RAN 7.2 (Benetel)
./build_image.sh -t release_26_10 -m <march> -v uhd     # ocudu-gnb-csi:release_26_10-uhd  USRP
./build_image.sh -t release_26_10 -m <march> -v zmq     # ocudu-gnb-csi:release_26_10-zmq  ZeroMQ (simulated RF)
./build_image.sh -s <clean ocudu source tree> ...         # no clone (the tree is copied, never modified)
```

- `dpdk`, `uhd`: upstream `docker/Dockerfile`, unmodified, targets `runtime-dpdk` / `runtime-uhd`, `COMPONENT=gnb`.
  They run as uid 1001 with file capabilities on `gnb` (`cap_sys_nice,cap_ipc_lock,cap_perfmon+ep`): the container
  needs these capabilities, otherwise `exec` fails with "operation not permitted":
  `docker run --cap-add SYS_NICE --cap-add IPC_LOCK --cap-add PERFMON ...` (Kubernetes: `securityContext`
  `capabilities.add`, or `privileged`).
- `zmq`: `docker/Dockerfile.zmq`, which reuses the upstream build scripts (upstream images have no ZeroMQ) and checks
  that `gnb` links libzmq. Runs as root.
- All variants contain `iproute2`, `ping` and `tcpdump`, like the former srsRAN images (the srsran-helm gNB chart runs
  `ip route`). For `dpdk` and `uhd`, `docker/Dockerfile.tools` adds them on top of the upstream image, tagged
  `<tag>-upstream`.
- Labels: `ocudu.tag`, `ocudu.revision`, `csi.logger.revision`, `csi.format_version`.
- `-m`: `-march` of the CPUs that run the gNB, not of the build host. It defaults to `native`, as upstream. One image
  for several CPU families: their common instruction set, e.g. `cascadelake` for Intel Cascade Lake and AMD Zen 4
  (`znver4` includes all of `cascadelake`). A CPU without the instructions crashes with SIGILL.
- Check: `docker run --rm [--cap-add ...] -e CSI_ENABLED=1 -e CSI_OUTPUT_DIR=/tmp <image> gnb --version` prints
  `[CSI] SRS logging to /tmp/csi_per_rb.csv ...`.
- No AW2S support in OCUDU (no driver; only UHD, ZeroMQ, Sidekiq, DIFI and O-RAN 7.2 fronthaul).

Validated on 2026-10-05: OCUDU `release_26_10` (e0db566aac), full `gnb` build with `-Werror` (gcc 13 on Ubuntu 24.04,
gcc 11.5 on Rocky 9), the three images built with `-m cascadelake` and started with the logger enabled. Not yet
validated on the testbed (ZMQ, Benetel, USRP runs).

## Tests

```bash
git clone --depth 1 -b release_26_10 https://gitlab.com/ocudu/ocudu.git
./patch_ocudu_srs_csi.sh ocudu
cmake -S ocudu -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_UHD=OFF -DENABLE_ZEROMQ=ON -DENABLE_PLUGINS=OFF
ninja -C build srs_estimator_benchmark            # builds the libraries the tests link with
tests/run_tests.sh $PWD/ocudu $PWD/build          # "ALL OK"
```

Linux only (g++, python3, ASan/UBSan/TSan runtimes). With conda in the PATH, CMake may pick conda's yaml-cpp/GTest
and the link of `gnb` fails: configure without conda, or add `-DCMAKE_IGNORE_PREFIX_PATH=<conda prefix>`.

1. `test_mapping`: RB index of the SRS pilots with OCUDU's `get_srs_information()`, 1080 SRS configurations.
2. `test_logger`: 4 threads, under ASan/UBSan and TSan; values encode their coordinates.
3. Overload: buffer full between two flushes, rows dropped and counted.
4. Antenna selection; disabled logger creates no file.
5. `test_ocudu_srs_e2e` + `check_ocudu_e2e.py`: a known channel is written on the SRS pilots of a resource grid,
   OCUDU's SRS estimator runs, and the logged rows are compared with the expected ones. It checks the RB indices,
   the physical RX ports (grid ports 1 and 3), the RNTI from `srs_context` (and 0 without context), the frame and slot,
   the values (comb 2 and 4, 1 and 4 symbols, frequency shift) and the TA compensation (0.3 us delay), and the
   multi-port SRS (a different channel per SRS port: 2 ports comb 2 and 4, 4 ports comb 2, 4 ports comb 4 on one
   comb and on interleaved combs; a plain per-RB mean fails these cases); in `rb` granularity and in `subcarrier`
   granularity with sampling 1, 2 and 4, where multi-port occasions are not logged (JSON header and column lists
   checked too).
6. `test_port_leakage`: leakage of the other SRS ports into the per-RB value, plain mean vs N-pilot groups, on random
   3-tap channels (ASan/UBSan); `average_per_rb_grouped` with groups of 1 is checked against `average_per_rb`.

`check_csv_v31.py FILE` also accepts OAI files. `test_latency.cpp` is informative only.
