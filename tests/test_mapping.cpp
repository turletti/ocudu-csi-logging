// RB mapping of the SRS pilots, with OCUDU's own get_srs_information(): pilot k holds the value of its carrier
// subcarrier (initial + k * comb). After average_per_rb(), per_rb[i] must be the mean subcarrier of the pilots of
// RB rb_start + i, i.e. lie inside that RB, and the RB count must be m_SRS.
#include "srs_csi_rb_logger.h"
#include "ocudu/ran/srs/srs_bandwidth_configuration.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"
#include <complex>
#include <cstdio>
#include <vector>
using namespace ocudu;
using csi_log::srs_csi_rb_logger;
int main()
{
  unsigned n_cases = 0, n_bad = 0;
  for (unsigned c_srs : {0u, 13u, 25u, 40u, 63u}) {
    for (unsigned b_srs = 0; b_srs != 4; ++b_srs) {
      auto bw = srs_configuration_get(c_srs, b_srs);
      if (!bw) {
        continue;
      }
      for (auto comb : {tx_comb_size::n2, tx_comb_size::n4}) {
        unsigned ncomb = static_cast<unsigned>(comb);
        for (unsigned k_tc = 0; k_tc != ncomb; ++k_tc) {
          for (unsigned shift : {0u, 3u, 7u}) {
            for (unsigned pos : {0u, 5u, 17u}) {
              srs_resource_configuration res = {};
              res.nof_antenna_ports   = srs_resource_configuration::one_two_four_enum::one;
              res.nof_symbols         = srs_nof_symbols::n1;
              res.start_symbol        = 13;
              res.configuration_index = c_srs;
              res.sequence_id         = 0;
              res.bandwidth_index     = b_srs;
              res.comb_size           = comb;
              res.comb_offset         = k_tc;
              res.cyclic_shift        = 0;
              res.freq_position       = pos;
              res.freq_shift          = shift;
              res.freq_hopping        = 0;
              res.hopping             = srs_group_or_sequence_hopping::neither;
              srs_information info    = get_srs_information(res, 0);
              std::vector<std::complex<float>> pilots(info.sequence_length);
              for (unsigned k = 0; k != info.sequence_length; ++k) {
                pilots[k] = float(info.mapping_initial_subcarrier + k * info.comb_size);
              }
              std::vector<std::complex<float>> per_rb(300);
              uint16_t rb_start = 0;
              unsigned n_rb     = srs_csi_rb_logger::average_per_rb(
                  pilots.data(), pilots.size(), info.mapping_initial_subcarrier, info.comb_size, per_rb.data(),
                  per_rb.size(), rb_start);
              bool ok = (n_rb == bw->m_srs) && (info.mapping_initial_subcarrier % 12 == k_tc);
              for (unsigned i = 0; ok && i != n_rb; ++i) {
                unsigned rb = unsigned(per_rb[i].real()) / 12;
                ok          = (rb == rb_start + i);
              }
              ++n_cases;
              if (!ok) {
                ++n_bad;
                std::printf("BAD c=%u b=%u comb=%u ktc=%u shift=%u pos=%u: m_srs=%u n_rb=%u init=%u\n", c_srs, b_srs,
                            ncomb, k_tc, shift, pos, bw->m_srs, n_rb, info.mapping_initial_subcarrier);
              }
            }
          }
        }
      }
    }
  }
  std::printf("mapping cases %u, bad %u\n", n_cases, n_bad);
  return n_bad != 0;
}
