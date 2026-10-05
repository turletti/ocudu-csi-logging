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
  /// \param[in] nb_antennas_rx, nb_ports_tx Totals written to the JSON header (taken from the first call).
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
  /// per RB (average_per_rb) and logged with log(); in "subcarrier" granularity each kept pilot is one row.
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
                  unsigned                   comb_size);

  /// Reports, once on stderr, that occasions with several SRS ports are not logged.
  void note_multiport_skipped();

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
