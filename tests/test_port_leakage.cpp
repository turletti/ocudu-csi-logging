// Leakage of the other SRS ports into the per-RB value of one port (multi-port SRS on one comb).
//
// With N ports on one comb, cyclic shifts n_cs_max / N apart, the LS estimate of port p on pilot k is
//   H_p[k] + sum_{q != p} H_q[k] exp(j 2 pi (q - p) k / N).
// Random 3-tap channels (delays up to about 300 ns, 30 kHz SCS) on 40 RB, 200 draws. The error of the per-RB value
// computed from the LS estimate, against the same averaging applied to the true channel of the port, is reported as
// an NMSE for the plain per-RB mean (group 1) and for the mean of N-pilot groups (average_per_rb_grouped).
// Fails if the grouped NMSE is above -25 dB, or if average_per_rb_grouped with group 1 differs from average_per_rb.
// Adapted from a simulation by T. Turletti (test_port_leakage.cpp).
//
// Build: g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I../src test_port_leakage.cpp ../src/srs_csi_rb_logger.cpp
#include "srs_csi_rb_logger.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using cf     = std::complex<float>;
using logger = csi_log::srs_csi_rb_logger;

int main()
{
  std::mt19937                    rng(1);
  std::normal_distribution<float> nd;
  const double                    pi   = std::acos(-1.0);
  int                             fail = 0;

  for (unsigned comb : {2U, 4U}) {
    for (unsigned nports : {2U, 4U}) {
      unsigned m_srs = 40, n = m_srs * 12 / comb, k0 = 5 * 12 + 1;
      double   e_plain = 0, p_plain = 0, e_grp = 0, p_grp = 0;
      for (int trial = 0; trial != 200; ++trial) {
        std::vector<std::vector<cf>> H(nports, std::vector<cf>(n));
        for (unsigned q = 0; q != nports; ++q) {
          cf     a[3];
          double d[3];
          for (int t = 0; t != 3; ++t) {
            a[t] = cf(nd(rng), nd(rng)) * static_cast<float>(std::exp(-t));
            d[t] = 100e-9 * t * (1 + 0.5 * nd(rng) * nd(rng));
          }
          for (unsigned k = 0; k != n; ++k) {
            double f = 30e3 * (k0 + comb * k);
            cf     h = 0;
            for (int t = 0; t != 3; ++t) {
              h += a[t] * cf(std::cos(-2 * pi * f * d[t]), std::sin(-2 * pi * f * d[t]));
            }
            H[q][k] = h;
          }
        }
        for (unsigned p = 0; p != nports; ++p) {
          std::vector<cf> lse(n);
          for (unsigned k = 0; k != n; ++k) {
            cf s = H[p][k];
            for (unsigned q = 0; q != nports; ++q) {
              if (q != p) {
                double ph = 2 * pi * (double(int(q) - int(p)) / nports) * k;
                s += H[q][k] * cf(std::cos(ph), std::sin(ph));
              }
            }
            lse[k] = s;
          }
          std::vector<cf>      ref(300), est(300), ref_old(300);
          std::vector<uint8_t> cnt(300);
          uint16_t             r0 = 0, r1 = 0, r2 = 0;
          // Plain per-RB mean: group 1, checked against average_per_rb.
          unsigned n1 = logger::average_per_rb_grouped(H[p].data(), n, k0, comb, 1, ref.data(), cnt.data(), 300, r0);
          unsigned n0 = logger::average_per_rb(H[p].data(), n, k0, comb, ref_old.data(), 300, r2);
          if (n1 != n0 || r0 != r2) {
            ++fail;
          }
          for (unsigned i = 0; i != n0 && i != n1; ++i) {
            if (std::abs(ref[i] - ref_old[i]) > 1e-5F * (1 + std::abs(ref_old[i]))) {
              ++fail;
            }
          }
          logger::average_per_rb_grouped(lse.data(), n, k0, comb, 1, est.data(), nullptr, 300, r1);
          for (unsigned i = 0; i != n1; ++i) {
            e_plain += std::norm(est[i] - ref[i]);
            p_plain += std::norm(ref[i]);
          }
          // Mean of N-pilot groups.
          unsigned ng = logger::average_per_rb_grouped(H[p].data(), n, k0, comb, nports, ref.data(), cnt.data(), 300, r0);
          logger::average_per_rb_grouped(lse.data(), n, k0, comb, nports, est.data(), nullptr, 300, r1);
          for (unsigned i = 0; i != ng; ++i) {
            if (cnt[i] != 0) {
              e_grp += std::norm(est[i] - ref[i]);
              p_grp += std::norm(ref[i]);
            }
          }
        }
      }
      double plain_db = 10 * std::log10(e_plain / p_plain), grp_db = 10 * std::log10(e_grp / p_grp);
      std::printf("  comb %u, %u ports on one comb: other-port leakage (NMSE) plain %.1f dB, grouped %.1f dB\n",
                  comb,
                  nports,
                  plain_db,
                  grp_db);
      if (grp_db > -25.0) {
        ++fail;
      }
    }
  }
  std::printf("port leakage: %s\n", fail == 0 ? "OK" : "FAILED");
  return fail == 0 ? 0 : 1;
}
