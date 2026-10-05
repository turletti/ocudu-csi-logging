// Worst log() call duration while the writer thread writes (aw2s-like rate: 133 RB, 2 antennas, every 500 us, 12 s).
#include "srs_csi_rb_logger.h"
#include <chrono>
#include <complex>
#include <cstdio>
#include <thread>
#include <vector>
using clk = std::chrono::steady_clock;
int main()
{
  auto* log = csi_log::srs_csi_rb_logger::get();
  std::vector<std::complex<float>> h(133, {1000.f, 1.f});
  double worst = 0; long calls = 0, over100 = 0;
  auto end = clk::now() + std::chrono::seconds(12);
  while (clk::now() < end) {
    for (uint8_t a = 0; a != 2; ++a) {
      auto t0 = clk::now();
      log->log(calls % 1024, 8, 0x4601, a, 0, 2, 1, 0, h.data(), h.size());
      double d = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
      if (calls > 0 && d > worst) worst = d;
      if (d > 100) ++over100;
    }
    ++calls;
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  std::printf("occasions %ld, rows %ld, worst log() call %.1f us, calls > 100 us: %ld\n", calls, calls * 2 * 133, worst, over100);
}
