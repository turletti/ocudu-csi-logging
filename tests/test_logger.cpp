// Host test of srs_csi_rb_logger: NTHREADS threads (like the SRS executor pool) log per-RB values that encode their
// own coordinates:  real = rb + 0.25,  imag = -(1000 * thread + 10 * ant + port) - 0.5.
// Usage: test_logger <rb_start> <n_rb> <occasions_per_thread> <sleep_us>
#include "srs_csi_rb_logger.h"
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <unistd.h>
using csi_log::srs_csi_rb_logger;
int main(int argc, char** argv)
{
  const unsigned rb_start = std::atoi(argv[1]), n_rb = std::atoi(argv[2]), nocc = std::atoi(argv[3]);
  const unsigned sleep_us = std::atoi(argv[4]);
  constexpr unsigned NTHREADS = 4, NANT = 2, NPORT = 1;
  srs_csi_rb_logger* log = srs_csi_rb_logger::get();
  if (log == nullptr) {
    std::printf("logger disabled\n");
    return 0;
  }
  std::vector<std::thread> th;
  for (unsigned t = 0; t != NTHREADS; ++t) {
    th.emplace_back([=] {
      std::vector<std::complex<float>> h(n_rb);
      for (unsigned occ = 0; occ != nocc; ++occ) {
        for (unsigned a = 0; a != NANT; ++a) {
          for (unsigned p = 0; p != NPORT; ++p) {
            for (unsigned i = 0; i != n_rb; ++i) {
              h[i] = {float(rb_start + i) + 0.25f, -float(1000 * t + 10 * a + p) - 0.5f};
            }
            log->log(occ % 1024, uint16_t(2 * t), uint16_t(0x4600 + t), uint8_t(a), uint8_t(p), NANT, NPORT,
                     uint16_t(rb_start), h.data(), n_rb);
          }
        }
        usleep(sleep_us);
      }
    });
  }
  for (auto& x : th) {
    x.join();
  }
  return 0; // the last rows are written by the atexit handler
}
