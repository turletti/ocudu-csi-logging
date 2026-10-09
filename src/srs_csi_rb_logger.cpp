/*
 * SRS channel logger (per RB or per pilot subcarrier), CSI CSV format v3.1 (see srs_csi_rb_logger.h).
 */

#include "srs_csi_rb_logger.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <thread>
#include <vector>

using namespace csi_log;

namespace {

/// One CSV row.
struct csi_row {
  uint32_t frame;
  uint16_t slot;
  uint16_t rnti;
  uint8_t  ant_rx;
  uint8_t  port_tx;
  uint16_t rb;
  uint8_t  sc; // subcarrier in the RB ("subcarrier" granularity only)
  float    re;
  float    im;
};

/// Maximum number of rows kept between two flushes (same as oai-csi-logging).
constexpr size_t   ring_capacity  = 1000000;
constexpr unsigned flush_period_s = 5;
/// Maximum number of RBs of a carrier (NR: 275).
constexpr unsigned max_nof_rb = 275;
// Maximum number of SRS ports of a UE (TS 38.331 nrofSRS-Ports), written as nb_ports_tx in the JSON header.
constexpr unsigned max_srs_ports = 4;

bool env_true(const char* name, bool def)
{
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return def;
  }
  return std::strcmp(v, "true") == 0 || std::strcmp(v, "1") == 0;
}

/// Parses "all" or a comma list of indices < 64 into a bit mask (all bits set for "all").
uint64_t parse_selection(const char* name)
{
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0' || std::strcmp(v, "all") == 0) {
    return ~uint64_t(0);
  }
  uint64_t    mask = 0;
  std::string s(v);
  size_t      pos = 0;
  while (pos <= s.size()) {
    size_t      comma = s.find(',', pos);
    std::string tok   = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    char*       end   = nullptr;
    long        idx   = std::strtol(tok.c_str(), &end, 10);
    if (end != tok.c_str() && idx >= 0 && idx < 64) {
      mask |= uint64_t(1) << idx;
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  return mask;
}

} // namespace

struct srs_csi_rb_logger::impl {
  std::FILE* file = nullptr;
  bool       include_header;
  bool       header_written = false;
  bool       per_subcarrier = false; // CSI_GRANULARITY=subcarrier
  unsigned   sc_sampling    = 1;     // CSI_SUBCARRIER_SAMPLING
  uint64_t   ant_mask;
  uint64_t   port_mask;
  int        flush_core = -1;

  std::mutex           mutex;
  std::vector<csi_row> rows;  // filled by log(), under mutex
  std::vector<csi_row> spare; // owned by the writer thread between two swaps
  uint64_t             dropped = 0;
  // JSON header fields, set by the first log() call (under mutex)
  bool     geometry_set   = false;
  uint8_t  nb_antennas_rx = 0;
  unsigned comb_size      = 0;  // comb of the first occasion ("subcarrier" granularity)
  uint64_t ants_seen      = 0;  // RX antennas of the rows logged so far (JSON header)

  bool                    closed = false; // set at process exit (under mutex): log() becomes a no-op
  std::atomic<bool>       stop{false};
  std::mutex              cv_mutex;
  std::condition_variable cv;
  std::thread             writer;
  std::atomic<bool>       multiport_warned{false};

  /// Writes the JSON and column headers once. ants: RX antennas of the rows logged so far, i.e. the antenna values of
  /// the file (physical RX port indices, not 0 .. nb_antenna_rx - 1). The SRS ports are not taken from the rows: the
  /// header is written at the first flush, often before the UE capabilities raise the number of SRS ports, so it
  /// gives the maximum (max_srs_ports) and the selected ports among them.
  void write_header(uint64_t ants_mask, unsigned comb)
  {
    if (header_written || !include_header) {
      return;
    }
    auto list = [](uint64_t m) {
      std::string s;
      for (unsigned i = 0; i != 64; ++i) {
        if ((m >> i) & 1U) {
          s += (s.empty() ? "" : ", ") + std::to_string(i);
        }
      }
      return s;
    };
    std::string ants = list(ants_mask), ports = list(port_mask & ((uint64_t(1) << max_srs_ports) - 1));
    const char* columns = per_subcarrier
                              ? "\"frame\", \"slot\", \"rnti\", \"ant_rx\", \"port_tx\", \"rb\", \"sc\", \"real\", \"imag\""
                              : "\"frame\", \"slot\", \"rnti\", \"ant_rx\", \"port_tx\", \"rb\", \"real\", \"imag\"";
    std::string granularity_fields =
        per_subcarrier ? "\"sc_values\": \"SRS LS pilots only (every comb-th subcarrier, no interpolation); sc = subcarrier "
                         "in the RB\", \"srs_comb\": " +
                             std::to_string(comb) + ", \"rb_value\": \"SRS LS pilot\", "
                       : std::string("\"rb_value\": \"complex mean of the SRS LS pilots of the RB; with N > 1 SRS ports, mean "
                                     "of the groups of N consecutive pilots centred in the RB (mean of N-pilot groups, "
                                     "port separation)\", ");
    std::fprintf(file,
                 "# { \"granularity\": \"%s\", \"nb_antenna_rx\": %u, \"nb_ports_tx\": %u, \"antenna_selection\": [ %s ], "
                 "\"port_selection\": [ %s ], \"subcarrier_sampling\": %u, \"format_version\": \"3.1\", "
                 "\"source\": \"ocudu-srs\", \"iq_format\": \"float\", "
                 "\"rb_index\": \"carrier CRB (SRS pilot subcarrier / 12)\", \"srs_symbols\": \"averaged\", "
                 "\"ta_compensated\": true, \"port_tx\": \"SRS port of the UE; the number of SRS ports (1, 2 or 4) "
                 "comes from the UE capabilities and changes after the RRC reconfiguration: nb_ports_tx is the maximum, "
                 "the port_tx column gives the port of each row\", %s"
                 "\"timestamp\": \"UTC, taken at flush time = end of the batch that follows the marker\", "
                 "\"flush_period_s\": %u, "
                 "\"columns\": [ %s ] }\n",
                 per_subcarrier ? "subcarrier" : "rb",
                 nb_antennas_rx,
                 max_srs_ports,
                 ants.c_str(),
                 ports.c_str(),
                 per_subcarrier ? sc_sampling : 1U,
                 granularity_fields.c_str(),
                 flush_period_s,
                 columns);
    std::fprintf(file,
                 per_subcarrier ? "frame,slot,rnti,ant_rx,port_tx,rb,sc,real,imag\n"
                                : "frame,slot,rnti,ant_rx,port_tx,rb,real,imag\n");
    header_written = true;
  }

  /// Takes the pending rows (O(1) swap) and writes them, with a UTC marker, outside the mutex.
  void flush_once()
  {
    uint64_t n_dropped = 0;
    uint64_t ants      = 0;
    unsigned comb      = 0;
    char     timestamp[32];
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (rows.empty() && dropped == 0) {
        return;
      }
      std::time_t now = std::time(nullptr);
      std::tm     tm_utc;
      gmtime_r(&now, &tm_utc); // end of the batch
      std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_utc);
      rows.swap(spare);
      n_dropped = dropped;
      dropped   = 0;
      ants      = ants_seen;
      comb      = comb_size;
    }
    std::fprintf(file, "# TIMESTAMP: %s\n", timestamp);
    if (n_dropped != 0) {
      std::fprintf(file, "# DROPPED: %llu\n", static_cast<unsigned long long>(n_dropped));
    }
    write_header(ants, comb);
    for (const csi_row& r : spare) {
      if (per_subcarrier) {
        std::fprintf(file,
                     "%u,%u,0x%04x,%u,%u,%u,%u,%.6g,%.6g\n",
                     r.frame,
                     r.slot,
                     r.rnti,
                     r.ant_rx,
                     r.port_tx,
                     r.rb,
                     r.sc,
                     static_cast<double>(r.re),
                     static_cast<double>(r.im));
        continue;
      }
      std::fprintf(file,
                   "%u,%u,0x%04x,%u,%u,%u,%.6g,%.6g\n",
                   r.frame,
                   r.slot,
                   r.rnti,
                   r.ant_rx,
                   r.port_tx,
                   r.rb,
                   static_cast<double>(r.re),
                   static_cast<double>(r.im));
    }
    std::fflush(file);
    // Drop the written pages from the page cache: in a container they are charged to the gNB memory cgroup, and a
    // cgroup kept at its limit by reclaiming them makes any gNB thread that needs a page wait for reclaim. Done here,
    // in the writer thread, never in a PHY thread. fdatasync first: DONTNEED does not drop dirty pages.
    const int fd = ::fileno(file);
    ::fdatasync(fd);
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    spare.clear(); // keeps the capacity (and the already faulted pages)
  }

  void run()
  {
    if (flush_core >= 0) {
      cpu_set_t cpuset;
      CPU_ZERO(&cpuset);
      CPU_SET(flush_core, &cpuset);
      if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
        std::fprintf(stderr, "[CSI] could not pin the writer thread to CPU %d\n", flush_core);
      }
    }
    std::unique_lock<std::mutex> wait_lock(cv_mutex);
    while (!stop.load()) {
      cv.wait_for(wait_lock, std::chrono::seconds(flush_period_s), [this] { return stop.load(); });
      wait_lock.unlock();
      flush_once();
      wait_lock.lock();
    }
  }

  /// Stops the writer and writes the last rows. Idempotent.
  void shutdown()
  {
    {
      std::lock_guard<std::mutex> lock(cv_mutex);
      stop.store(true);
    }
    cv.notify_all();
    if (writer.joinable()) {
      writer.join();
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (closed) {
        return;
      }
      closed = true;
    }
    flush_once(); // rows logged after the last periodic flush
    std::fclose(file);
  }
};

namespace {
// The logger is never destroyed: PHY threads may still call log() while the process exits.
srs_csi_rb_logger::impl* g_impl = nullptr;
} // namespace

srs_csi_rb_logger* srs_csi_rb_logger::get()
{
  // Created once, on first use, and never destroyed; at process exit the writer stops and the last rows are written.
  static srs_csi_rb_logger* instance = []() -> srs_csi_rb_logger* {
    if (!env_true("CSI_ENABLED", false)) {
      return nullptr;
    }
    const char* dir_env = std::getenv("CSI_OUTPUT_DIR");
    std::string dir     = (dir_env != nullptr && dir_env[0] != '\0') ? dir_env : "/data/csi";
    ::mkdir(dir.c_str(), 0755);
    std::string path = dir + "/csi_per_rb.csv";
    std::FILE*  f    = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
      std::fprintf(stderr, "[CSI] cannot open %s (%s): SRS CSI logging disabled\n", path.c_str(), std::strerror(errno));
      return nullptr;
    }
    auto* p           = new impl();
    p->file           = f;
    p->include_header = env_true("CSI_INCLUDE_HEADER", true);
    p->ant_mask       = parse_selection("CSI_ANTENNA_SELECTION");
    p->port_mask      = parse_selection("CSI_PORT_SELECTION");
    if (const char* g = std::getenv("CSI_GRANULARITY"); g != nullptr && std::strcmp(g, "subcarrier") == 0) {
      p->per_subcarrier = true;
    } else if (g != nullptr && g[0] != '\0' && std::strcmp(g, "rb") != 0) {
      std::fprintf(stderr, "[CSI] CSI_GRANULARITY=%s unknown, using rb\n", g);
    }
    if (const char* s = std::getenv("CSI_SUBCARRIER_SAMPLING"); s != nullptr && s[0] != '\0') {
      int n = std::atoi(s);
      if (n >= 1 && n <= 12) {
        p->sc_sampling = static_cast<unsigned>(n);
      } else {
        std::fprintf(stderr, "[CSI] CSI_SUBCARRIER_SAMPLING must be 1-12, using 1\n");
      }
    }
    if (const char* core = std::getenv("CSI_FLUSH_CORE"); core != nullptr && core[0] != '\0') {
      p->flush_core = std::atoi(core);
    }
    // Allocate and touch both buffers now, so that log() never allocates nor page-faults.
    p->rows.resize(ring_capacity);
    p->rows.clear();
    p->spare.resize(ring_capacity);
    p->spare.clear();
    auto* logger = new srs_csi_rb_logger(p);
    p->writer    = std::thread([p] { p->run(); });
    g_impl       = p;
    std::atexit([] { g_impl->shutdown(); });
    std::fprintf(stderr,
                 "[CSI] SRS logging to %s (CSI CSV format v3.1, granularity %s, subcarrier sampling %u)\n",
                 path.c_str(),
                 p->per_subcarrier ? "subcarrier" : "rb",
                 p->sc_sampling);
    // A file on tmpfs is in RAM (and charged to the container memory as shmem): at several MB/s it ends in an OOM.
    // An overlay may also sit on tmpfs (live systems): the underlying filesystem cannot be seen from here.
    constexpr long tmpfs_magic = 0x01021994, overlay_magic = 0x794c7630;
    if (struct statfs fs; ::statfs(dir.c_str(), &fs) == 0 && (fs.f_type == tmpfs_magic || fs.f_type == overlay_magic)) {
      std::fprintf(stderr,
                   "[CSI] WARNING: %s is on %s: the CSI file may be in RAM and counted in the container memory; "
                   "use a directory on a disk\n",
                   dir.c_str(),
                   fs.f_type == tmpfs_magic ? "tmpfs" : "an overlay filesystem");
    }
    return logger;
  }();
  return instance;
}

namespace {
// Create the logger when the program starts (dynamic initialisation, before main) instead of on the first SRS
// occasion: preallocating and touching the two buffers takes ~20 ms, which must not happen in a PHY thread. This also
// keeps the writer thread out of the PHY thread affinity (it inherits the affinity of the main thread at start-up).
[[maybe_unused]] const bool created_at_startup = (srs_csi_rb_logger::get(), true);
} // namespace

srs_csi_rb_logger::srs_csi_rb_logger(impl* p) : pimpl(p) {}

srs_csi_rb_logger::~srs_csi_rb_logger()
{
  pimpl->shutdown();
}

void srs_csi_rb_logger::log(uint32_t                   frame,
                            uint16_t                   slot,
                            uint16_t                   rnti,
                            uint8_t                    ant_rx,
                            uint8_t                    port_tx,
                            uint8_t                    nb_antennas_rx,
                            uint8_t                    nb_ports_tx,
                            uint16_t                   rb_start,
                            const std::complex<float>* per_rb,
                            unsigned                   n_rb)
{
  (void)nb_ports_tx; // the JSON header gives the maximum number of SRS ports, see write_header()
  if (per_rb == nullptr || n_rb == 0 || ant_rx >= 64 || port_tx >= 64 || ((pimpl->ant_mask >> ant_rx) & 1U) == 0 ||
      ((pimpl->port_mask >> port_tx) & 1U) == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(pimpl->mutex);
  if (pimpl->closed) {
    return;
  }
  if (!pimpl->geometry_set) {
    pimpl->nb_antennas_rx = nb_antennas_rx;
    pimpl->geometry_set   = true;
  }
  pimpl->ants_seen |= uint64_t(1) << ant_rx;
  for (unsigned i = 0; i != n_rb; ++i) {
    if (pimpl->rows.size() >= ring_capacity) {
      pimpl->dropped += n_rb - i;
      break;
    }
    pimpl->rows.push_back(csi_row{frame,
                                  slot,
                                  rnti,
                                  ant_rx,
                                  port_tx,
                                  static_cast<uint16_t>(rb_start + i),
                                  0,
                                  per_rb[i].real(),
                                  per_rb[i].imag()});
  }
}

void srs_csi_rb_logger::log_pilots(uint32_t                   frame,
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
                                   unsigned                   group)
{
  if (!pimpl->per_subcarrier) {
    // Averaging outside the mutex, on the stack of the calling (PHY) thread.
    std::array<std::complex<float>, max_nof_rb + 1> per_rb;
    uint16_t                                        rb_start = 0;
    if (group <= 1) {
      unsigned n_rb =
          average_per_rb(pilots, nof_pilots, initial_subcarrier, comb_size, per_rb.data(), per_rb.size(), rb_start);
      log(frame, slot, rnti, ant_rx, port_tx, nb_antennas_rx, nb_ports_tx, rb_start, per_rb.data(), n_rb);
      return;
    }
    std::array<uint8_t, max_nof_rb + 1> counts;
    unsigned                            n_rb = average_per_rb_grouped(pilots,
                                                   nof_pilots,
                                                   initial_subcarrier,
                                                   comb_size,
                                                   group,
                                                   per_rb.data(),
                                                   counts.data(),
                                                   per_rb.size(),
                                                   rb_start);
    // Log the runs of RBs that contain at least one group (an RB without group has no value).
    for (unsigned i = 0; i != n_rb;) {
      if (counts[i] == 0) {
        ++i;
        continue;
      }
      unsigned j = i;
      while (j != n_rb && counts[j] != 0) {
        ++j;
      }
      log(frame,
          slot,
          rnti,
          ant_rx,
          port_tx,
          nb_antennas_rx,
          nb_ports_tx,
          static_cast<uint16_t>(rb_start + i),
          per_rb.data() + i,
          j - i);
      i = j;
    }
    return;
  }
  if (group > 1) {
    // A single pilot contains the other SRS ports: nothing to log in subcarrier granularity.
    if (!pimpl->multiport_warned.exchange(true)) {
      std::fprintf(stderr,
                   "[CSI] SRS occasions with more than one SRS port are not logged in subcarrier granularity: a single "
                   "pilot does not separate the ports (use CSI_GRANULARITY=rb)\n");
    }
    return;
  }
  constexpr unsigned nre = 12;
  if (pilots == nullptr || nof_pilots == 0 || comb_size == 0 || ant_rx >= 64 || port_tx >= 64 ||
      ((pimpl->ant_mask >> ant_rx) & 1U) == 0 || ((pimpl->port_mask >> port_tx) & 1U) == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(pimpl->mutex);
  if (pimpl->closed) {
    return;
  }
  if (!pimpl->geometry_set) {
    pimpl->nb_antennas_rx = nb_antennas_rx;
    pimpl->comb_size      = comb_size;
    pimpl->geometry_set   = true;
  }
  pimpl->ants_seen |= uint64_t(1) << ant_rx;
  for (unsigned k = 0; k != nof_pilots; ++k) {
    unsigned subcarrier = initial_subcarrier + k * comb_size;
    auto     sc         = static_cast<uint8_t>(subcarrier % nre);
    // Index of the pilot in its RB: the pilots of an RB are at sc = k_TC + j * comb, k_TC < comb.
    if ((sc / comb_size) % pimpl->sc_sampling != 0) {
      continue;
    }
    if (pimpl->rows.size() >= ring_capacity) {
      ++pimpl->dropped;
      continue;
    }
    pimpl->rows.push_back(csi_row{frame,
                                  slot,
                                  rnti,
                                  ant_rx,
                                  port_tx,
                                  static_cast<uint16_t>(subcarrier / nre),
                                  sc,
                                  pilots[k].real(),
                                  pilots[k].imag()});
  }
}

unsigned srs_csi_rb_logger::average_per_rb(const std::complex<float>* pilots,
                                           unsigned                   nof_pilots,
                                           unsigned                   initial_subcarrier,
                                           unsigned                   comb_size,
                                           std::complex<float>*       per_rb,
                                           unsigned                   capacity,
                                           uint16_t&                  rb_start)
{
  constexpr unsigned nre = 12;
  rb_start               = static_cast<uint16_t>(initial_subcarrier / nre);
  if (pilots == nullptr || nof_pilots == 0 || comb_size == 0 || capacity == 0) {
    return 0;
  }
  unsigned            n_rb  = 0;
  unsigned            count = 0;
  std::complex<float> sum   = 0;
  unsigned            cur   = rb_start;
  for (unsigned k = 0; k != nof_pilots; ++k) {
    unsigned rb = (initial_subcarrier + k * comb_size) / nre;
    if (rb != cur) {
      if (n_rb == capacity) {
        return n_rb;
      }
      per_rb[n_rb++] = sum / static_cast<float>(count);
      // RBs without pilots cannot occur with comb 2 or 4 (12 / comb pilots per RB); keep indices contiguous anyway
      for (++cur; cur < rb && n_rb < capacity; ++cur) {
        per_rb[n_rb++] = 0;
      }
      cur   = rb;
      sum   = 0;
      count = 0;
    }
    sum += pilots[k];
    ++count;
  }
  if (count != 0 && n_rb < capacity) {
    per_rb[n_rb++] = sum / static_cast<float>(count);
  }
  return n_rb;
}

unsigned srs_csi_rb_logger::average_per_rb_grouped(const std::complex<float>* pilots,
                                                   unsigned                   nof_pilots,
                                                   unsigned                   initial_subcarrier,
                                                   unsigned                   comb_size,
                                                   unsigned                   group,
                                                   std::complex<float>*       per_rb,
                                                   uint8_t*                   counts,
                                                   unsigned                   capacity,
                                                   uint16_t&                  rb_start)
{
  constexpr unsigned nre = 12;
  rb_start               = static_cast<uint16_t>(initial_subcarrier / nre);
  if (pilots == nullptr || per_rb == nullptr || group == 0 || comb_size == 0 || nof_pilots < group || capacity == 0) {
    return 0;
  }
  unsigned n_groups = nof_pilots / group;
  // Centre subcarrier of group m, in half subcarriers to stay integer: pilots m * group .. m * group + group - 1.
  auto centre2 = [initial_subcarrier, comb_size, group](unsigned m) {
    return 2 * initial_subcarrier + comb_size * (2 * m * group + group - 1);
  };
  unsigned first_rb = centre2(0) / (2 * nre);
  unsigned n_rb     = centre2(n_groups - 1) / (2 * nre) - first_rb + 1;
  n_rb              = std::min(n_rb, std::min(capacity, max_nof_rb));
  // Group counts on the stack (at most a few groups per RB), whether or not the caller asked for them.
  std::array<uint8_t, max_nof_rb> cnt{};
  for (unsigned i = 0; i != n_rb; ++i) {
    per_rb[i] = 0;
  }
  for (unsigned m = 0; m != n_groups; ++m) {
    unsigned rb = centre2(m) / (2 * nre) - first_rb;
    if (rb >= n_rb) {
      break;
    }
    std::complex<float> sum = 0;
    for (unsigned g = 0; g != group; ++g) {
      sum += pilots[m * group + g];
    }
    per_rb[rb] += sum / static_cast<float>(group);
    ++cnt[rb];
  }
  for (unsigned i = 0; i != n_rb; ++i) {
    if (cnt[i] != 0) {
      per_rb[i] /= static_cast<float>(cnt[i]);
    }
    if (counts != nullptr) {
      counts[i] = cnt[i];
    }
  }
  rb_start = static_cast<uint16_t>(first_rb);
  return n_rb;
}
