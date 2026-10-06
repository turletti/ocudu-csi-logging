/*
 * SRS channel logger (per RB or per pilot subcarrier) writing the CSI CSV format v3.1 of oai-csi-logging
 * (https://github.com/turletti/oai-csi-logging), so that OCUDU (ex srsRAN Project) and OAI runs are collected and
 * analysed with the same tools (5g_ansible collection/live view, Streamlit visualizer, csi_diag.py).
 *
 * Standard library only, in its own namespace (no OCUDU/srsRAN dependency), so that it can be tested on its own and
 * applied to OCUDU by patch_ocudu_srs_csi.sh without depending on the project namespace.
 */

#pragma once

#include <complex>
#include <cstdint>

namespace csi_log {

/// \brief Process-wide SRS channel logger (per RB, or per pilot subcarrier with CSI_GRANULARITY=subcarrier).
///
/// Enabled when the environment variable CSI_ENABLED is "true" or "1" at program start (the logger is created
/// before main(), see srs_csi_rb_logger.cpp).
/// Other environment variables:
///   CSI_OUTPUT_DIR          output directory, file csi_per_rb.csv (default /data/csi, truncated at start)
///   CSI_GRANULARITY         "rb" (default): complex mean of the SRS pilots of each RB
///                           "subcarrier": one row per SRS pilot, with its subcarrier sc (0..11) in the RB. Only the
///                           pilots are logged (every comb-th subcarrier): there is no interpolation.
///   CSI_SUBCARRIER_SAMPLING "subcarrier" granularity only: keep 1 pilot out of N in each RB, i.e. the pilots whose
///                           index in the RB (sc / comb) is a multiple of N (1..12, default 1 = all pilots)
///   CSI_ANTENNA_SELECTION   "all" (default) or a comma list of RX antenna indices (physical RX ports)
///   CSI_PORT_SELECTION      "all" (default) or a comma list of SRS port indices
///   CSI_INCLUDE_HEADER      write the JSON + column header (default true)
///   CSI_FLUSH_CORE          optional CPU the writer thread is pinned to (default: not pinned)
///
/// Real-time behaviour: log() only appends rows to a preallocated buffer under a mutex. A writer thread swaps the
/// buffer every 5 s (the swap is O(1)) and writes it to the file outside the mutex. When the buffer is full, rows are
/// dropped and counted ("# DROPPED: n" line).
class srs_csi_rb_logger
{
public:
  /// Returns the logger, or nullptr when CSI logging is disabled.
  static srs_csi_rb_logger* get();

  /// \brief Logs one SRS occasion of one (RX antenna, SRS port).
  /// \param[in] per_rb Complex channel value of each RB, for the carrier CRBs rb_start .. rb_start + n_rb - 1.
  /// \param[in] nb_antennas_rx Total written to the JSON header (taken from the first call).
  /// \param[in] nb_ports_tx    Number of SRS ports of the occasion. Not written: it can change after the RRC
  ///                           reconfiguration, so the header gives the maximum (4) and port_tx gives the port.
  void log(uint32_t                   frame,
           uint16_t                   slot,
           uint16_t                   rnti,
           uint8_t                    ant_rx,
           uint8_t                    port_tx,
           uint8_t                    nb_antennas_rx,
           uint8_t                    nb_ports_tx,
           uint16_t                   rb_start,
           const std::complex<float>* per_rb,
           unsigned                   n_rb);

  /// \brief Logs one SRS occasion of one (RX antenna, SRS port) from its LS pilots, according to CSI_GRANULARITY.
  ///
  /// Pilot k is at carrier subcarrier initial_subcarrier + k * comb_size. In "rb" granularity the pilots are averaged
  /// per RB and logged with log(); in "subcarrier" granularity each kept pilot is one row.
  /// \param[in] group Number of consecutive pilots whose mean cancels the other SRS ports sharing the comb: 1 for
  ///                  one SRS port, otherwise the number of ports on the comb (average_per_rb_grouped). In
  ///                  "subcarrier" granularity a single pilot does not separate the ports: occasions with group > 1
  ///                  are not logged (reported once on stderr).
  void log_pilots(uint32_t                   frame,
                  uint16_t                   slot,
                  uint16_t                   rnti,
                  uint8_t                    ant_rx,
                  uint8_t                    port_tx,
                  uint8_t                    nb_antennas_rx,
                  uint8_t                    nb_ports_tx,
                  const std::complex<float>* pilots,
                  unsigned                   nof_pilots,
                  unsigned                   initial_subcarrier,
                  unsigned                   comb_size,
                  unsigned                   group = 1);

  /// \brief Averages SRS pilots per resource block (complex mean of the pilots of each RB).
  ///
  /// Pilot k is at carrier subcarrier initial_subcarrier + k * comb_size. Writes one value per RB into per_rb, from
  /// RB initial_subcarrier / 12, and returns the number of RBs written (at most capacity).
  static unsigned average_per_rb(const std::complex<float>* pilots,
                                 unsigned                   nof_pilots,
                                 unsigned                   initial_subcarrier,
                                 unsigned                   comb_size,
                                 std::complex<float>*       per_rb,
                                 unsigned                   capacity,
                                 uint16_t&                  rb_start);

  /// \brief Averages SRS pilots per resource block by groups of \c group consecutive pilots, to separate SRS ports.
  ///
  /// With N SRS ports on the same comb, the cyclic shifts of the ports are n_cs_max / N apart (TS 38.211 6.4.1.4.2),
  /// so the LS estimate of port p on pilot k is H_p[k] + sum_{q != p} H_q[k] exp(j 2 pi (q - p) k / N), k counted from
  /// the first pilot of the sequence. The mean of N consecutive pilots (k = m N .. m N + N - 1) cancels the other
  /// ports when the channel is flat over them. Group m is assigned to the RB of its centre subcarrier; the value of an
  /// RB is the mean of its groups. counts[i] (if not null) receives the number of groups of RB rb_start + i: 0 when no
  /// group centre falls in it (possible with comb 4 and 4 ports on the same comb), the value is then 0 and must not
  /// be logged. Leftover pilots (nof_pilots % group) are ignored. No heap allocation. Returns the number of RBs
  /// written (at most capacity and max 275).
  static unsigned average_per_rb_grouped(const std::complex<float>* pilots,
                                         unsigned                   nof_pilots,
                                         unsigned                   initial_subcarrier,
                                         unsigned                   comb_size,
                                         unsigned                   group,
                                         std::complex<float>*       per_rb,
                                         uint8_t*                   counts,
                                         unsigned                   capacity,
                                         uint16_t&                  rb_start);

  ~srs_csi_rb_logger();
  srs_csi_rb_logger(const srs_csi_rb_logger&)            = delete;
  srs_csi_rb_logger& operator=(const srs_csi_rb_logger&) = delete;

  /// Opaque implementation.
  struct impl;

private:
  explicit srs_csi_rb_logger(impl* p);
  impl* pimpl;
};

} // namespace csi_log
