// End-to-end test of the per-RB SRS CSI logger inside OCUDU's SRS estimator (patched tree).
//
// A known channel is written on the SRS pilots of a resource grid; the estimator is run with CSI_ENABLED and the
// values that it logs are compared (by check_ocudu_e2e.py) with the expected ones, written by this program to
// <out_dir>/expected.csv (per RB) and <out_dir>/expected_sc.csv (per SRS pilot, for CSI_GRANULARITY=subcarrier).
// Each case uses its own slot so that the checker can separate them.
//
// Cases (rx ports are physical grid ports {1, 3}, so ant_rx must be 1 and 3, not 0 and 1):
//   A: comb 2, 1 symbol,  no delay       -> logged value = H(rb)                              (strict)
//   B: comb 4, 4 symbols, freq shift 5   -> logged value = H(rb)                              (strict)
//   C: comb 2, 1 symbol,  delay 0.3 us   -> TA compensated: |value| = |H|, phase nearly flat  (loose)
//   D: no srs_context                     -> rnti 0x0000
//   E: comb 2, 2 SRS ports                -> one row per port, value = H(port, rb)   (groups of 2 pilots)
//   F: comb 4, 2 SRS ports                -> idem (groups of 2 pilots straddle RBs)   (OCUDU default comb)
//   G: comb 4, 4 SRS ports, cyclic shift 6 -> interleaved combs, groups of 2 pilots
//   H: comb 2, 4 SRS ports                -> groups of 4 pilots
//   I: comb 4, 4 SRS ports, cyclic shift 0 -> groups of 4 pilots (16 subcarriers): RBs without a group centre are
//                                             not logged
//   The channel differs between SRS ports (amplitude and phase), so a plain per-RB mean, which leaves the other
//   ports in the estimate, fails the comparison. Multi-port occasions are not logged in subcarrier granularity.
//
// Usage: test_ocudu_srs_e2e <out_dir>     (CSI_ENABLED=1 CSI_OUTPUT_DIR=<out_dir> must be set)

#include "ocudu/phy/generic_functions/generic_functions_factories.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_configuration.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_factory.h"
#include "ocudu/phy/upper/signal_processors/srs/srs_estimator_result.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/srs/srs_information.h"
#include "ocudu/support/error_handling.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ocudu;

namespace {

constexpr unsigned           nof_grid_ports = 4;
constexpr unsigned           rx_ports[]     = {1, 3};
constexpr subcarrier_spacing scs            = subcarrier_spacing::kHz30;

/// Known channel of RX port p and SRS port tx at carrier RB rb (constant phase per RB: no delay).
cf_t channel(unsigned p, unsigned tx, unsigned rb)
{
  float amp   = (p == 1 ? 1.0F : 0.5F) * (1.0F + 0.004F * static_cast<float>(rb)) * (1.0F - 0.15F * static_cast<float>(tx));
  float phase = (p == 1 ? 0.7F : -2.1F) + 1.3F * static_cast<float>(tx);
  return std::polar(amp, phase);
}

struct test_case {
  char         name;
  unsigned     slot_index;
  tx_comb_size comb;
  unsigned     nof_symbols;
  unsigned     freq_shift;
  double       delay_s;
  bool         with_context;
  unsigned     nof_tx_ports;
  unsigned     cyclic_shift;
};

} // namespace

int main(int argc, char** argv)
{
  report_fatal_error_if_not(argc == 2, "usage: test_ocudu_srs_e2e <out_dir>");
  std::string out_dir = argv[1];

  std::shared_ptr<low_papr_sequence_generator_factory> seq_factory = create_low_papr_sequence_generator_sw_factory();
  std::shared_ptr<dft_processor_factory>               dft_factory = create_dft_processor_factory_fftw_slow();
  if (!dft_factory) {
    dft_factory = create_dft_processor_factory_generic();
  }
  std::shared_ptr<time_alignment_estimator_factory> ta_factory = create_time_alignment_estimator_dft_factory(dft_factory);
  std::shared_ptr<srs_estimator_factory>            est_factory =
      create_srs_estimator_generic_factory(seq_factory, ta_factory, nullptr, MAX_NOF_PRBS);
  report_fatal_error_if_not(est_factory, "srs estimator factory");
  std::unique_ptr<srs_estimator>               estimator = est_factory->create();
  std::unique_ptr<low_papr_sequence_generator> seq_gen   = seq_factory->create();
  std::shared_ptr<resource_grid_factory>       rg_factory = create_resource_grid_factory();
  unsigned                                     nof_subc   = MAX_NOF_PRBS * NOF_SUBCARRIERS_PER_RB;
  std::unique_ptr<resource_grid> grid = rg_factory->create(nof_grid_ports, get_nsymb_per_slot(cyclic_prefix::NORMAL), nof_subc);
  report_fatal_error_if_not(estimator && seq_gen && grid, "objects");

  const test_case cases[] = {{'A', 1, tx_comb_size::n2, 1, 0, 0.0, true, 1, 0},
                             {'B', 2, tx_comb_size::n4, 4, 5, 0.0, true, 1, 0},
                             {'C', 3, tx_comb_size::n2, 1, 0, 0.3e-6, true, 1, 0},
                             {'D', 4, tx_comb_size::n2, 1, 0, 0.0, false, 1, 0},
                             {'E', 5, tx_comb_size::n2, 1, 0, 0.0, true, 2, 0},
                             {'F', 6, tx_comb_size::n4, 1, 0, 0.0, true, 2, 0},
                             {'G', 7, tx_comb_size::n4, 1, 0, 0.0, true, 4, 6},
                             {'H', 8, tx_comb_size::n2, 1, 0, 0.0, true, 4, 0},
                             {'I', 9, tx_comb_size::n4, 1, 0, 0.0, true, 4, 0}};

  std::FILE* expected = std::fopen((out_dir + "/expected.csv").c_str(), "w");
  report_fatal_error_if_not(expected != nullptr, "cannot write expected.csv");
  std::fprintf(expected, "case,frame,slot,rnti,ant_rx,port_tx,rb,real,imag\n");
  std::FILE* expected_sc = std::fopen((out_dir + "/expected_sc.csv").c_str(), "w");
  report_fatal_error_if_not(expected_sc != nullptr, "cannot write expected_sc.csv");
  std::fprintf(expected_sc, "case,frame,slot,rnti,ant_rx,port_tx,rb,sc,real,imag\n");

  for (const test_case& tc : cases) {
    srs_resource_configuration res = {};
    res.nof_antenna_ports   = static_cast<srs_resource_configuration::one_two_four_enum>(tc.nof_tx_ports);
    res.nof_symbols         = static_cast<srs_nof_symbols>(tc.nof_symbols);
    res.start_symbol        = 14 - tc.nof_symbols;
    res.configuration_index = 40; // m_SRS = 160 RB with B_SRS = 0
    res.sequence_id         = 17;
    res.bandwidth_index     = 0;
    res.comb_size           = tc.comb;
    res.comb_offset         = 1;
    res.cyclic_shift        = tc.cyclic_shift;
    res.freq_position       = 0;
    res.freq_shift          = tc.freq_shift;
    res.freq_hopping        = 0;
    res.hopping             = srs_group_or_sequence_hopping::neither;
    res.periodicity         = srs_resource_configuration::periodicity_and_offset{10, 0};

    srs_estimator_configuration config;
    config.slot        = slot_point(to_numerology_value(scs), 123, tc.slot_index);
    config.slot_offset = 0;
    config.resource    = res;
    config.ports       = {static_cast<uint8_t>(rx_ports[0]), static_cast<uint8_t>(rx_ports[1])};
    if (tc.with_context) {
      config.context = srs_context(0, to_rnti(0x4601));
    }

    // Grid: sum over SRS ports of H(rb) * sequence(port) on the pilots, with an optional delay.
    grid->set_all_zero();
    for (unsigned p : rx_ports) {
      for (unsigned l = res.start_symbol.value(); l != res.start_symbol.value() + tc.nof_symbols; ++l) {
        std::vector<cf_t> symbol(nof_subc, cf_t(0));
        for (unsigned i_tx = 0; i_tx != tc.nof_tx_ports; ++i_tx) {
          srs_information   info = get_srs_information(res, i_tx);
          std::vector<cf_t> seq(info.sequence_length);
          seq_gen->generate(seq, info.sequence_group, info.sequence_number, info.n_cs, info.n_cs_max);
          for (unsigned k = 0; k != info.sequence_length; ++k) {
            unsigned sc    = info.mapping_initial_subcarrier + k * info.comb_size;
            double   ph    = -2.0 * M_PI * static_cast<double>(sc) * scs_to_khz(scs) * 1e3 * tc.delay_s;
            cf_t     delay = std::polar(1.0F, static_cast<float>(ph));
            symbol[sc] += channel(p, i_tx, sc / NOF_SUBCARRIERS_PER_RB) * delay * seq[k];
          }
        }
        grid->get_writer().put(p, l, 0, symbol);
      }
    }

    srs_estimator_result result = estimator->estimate(grid->get_reader(), config);
    std::printf("case %c: TA %.3f us (true %.3f us)\n", tc.name, result.time_alignment.time_alignment * 1e6, tc.delay_s * 1e6);

    // Expected rows. Pilots averaged by groups of `group` (the number of SRS ports sharing a comb), each group in the
    // RB of its centre subcarrier (written here independently of the logger).
    bool     interleaved = (tc.nof_tx_ports == 4) && (tc.cyclic_shift >= ((tc.comb == tx_comb_size::n4) ? 6U : 4U));
    unsigned group       = (tc.nof_tx_ports == 1) ? 1 : (interleaved ? 2 : tc.nof_tx_ports);
    for (unsigned i_tx = 0; i_tx != tc.nof_tx_ports; ++i_tx) {
      srs_information info = get_srs_information(res, i_tx);
      std::vector<bool> rb_logged(MAX_NOF_PRBS, false);
      for (unsigned m = 0; m != info.sequence_length / group; ++m) {
        double first  = info.mapping_initial_subcarrier + m * group * info.comb_size;
        double centre = first + 0.5 * (group - 1) * info.comb_size;
        rb_logged[static_cast<unsigned>(centre / NOF_SUBCARRIERS_PER_RB)] = true;
      }
      for (unsigned p : rx_ports) {
        // Per pilot (single-port occasions only: multi-port occasions are not logged in subcarrier granularity).
        for (unsigned k = 0; group == 1 && k != info.sequence_length; ++k) {
          unsigned sc = info.mapping_initial_subcarrier + k * info.comb_size;
          cf_t     h  = channel(p, i_tx, sc / NOF_SUBCARRIERS_PER_RB);
          std::fprintf(expected_sc,
                       "%c,%u,%u,0x%04x,%u,%u,%u,%u,%.6g,%.6g\n",
                       tc.name,
                       config.slot.sfn(),
                       config.slot.slot_index(),
                       tc.with_context ? 0x4601U : 0U,
                       p,
                       i_tx,
                       static_cast<unsigned>(sc / NOF_SUBCARRIERS_PER_RB),
                       static_cast<unsigned>(sc % NOF_SUBCARRIERS_PER_RB),
                       h.real(),
                       h.imag());
        }
        for (unsigned rb = 0; rb != MAX_NOF_PRBS; ++rb) {
          if (!rb_logged[rb]) {
            continue;
          }
          cf_t h = channel(p, i_tx, rb);
          std::fprintf(expected,
                       "%c,%u,%u,0x%04x,%u,%u,%u,%.6g,%.6g\n",
                       tc.name,
                       config.slot.sfn(),
                       config.slot.slot_index(),
                       tc.with_context ? 0x4601U : 0U,
                       p,
                       i_tx,
                       rb,
                       h.real(),
                       h.imag());
        }
      }
    }
  }
  std::fclose(expected);
  std::fclose(expected_sc);
  return 0; // the logger writes its last rows at exit
}
