/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/tools/trace_replay/orchestrator.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/subprocess.h"
#include "perfetto/ext/base/watchdog_posix.h"

#include "src/tools/trace_replay/config_forger.h"
#include "src/tools/trace_replay/proc_monitor.h"
#include "src/tools/trace_replay/replay_file.h"
#include "src/tools/trace_replay/trace_analyzer.h"

namespace perfetto {
namespace trace_replay {

namespace {

std::string DefaultOutDir() {
  char tmpl[] = "/tmp/replay.XXXXXX";
  char* p = mkdtemp(tmpl);
  if (!p) {
    char fb[64];
    snprintf(fb, sizeof(fb), "/tmp/replay.%d", static_cast<int>(getpid()));
    return fb;
  }
  return std::string(p);
}

// Reads /proc/<pid>/stat into `out`. Returns false if the process is gone.
bool ReadTracedStats(int pid, base::ProcStat* out) {
  if (pid <= 0)
    return false;
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  base::ScopedFile fd(base::OpenFile(path, O_RDONLY));
  if (!fd)
    return false;
  return base::ReadProcStat(fd.get(), out);
}

long ReadTracedRssKb(int pid) {
  if (pid <= 0)
    return -1;
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/statm", pid);
  std::string s;
  if (!base::ReadFile(path, &s))
    return -1;
  long sz = 0, rss = 0;
  if (sscanf(s.c_str(), "%ld %ld", &sz, &rss) != 2)
    return -1;
  return rss * (sysconf(_SC_PAGESIZE) / 1024);
}

bool MakeDirP(const std::string& path) {
  if (mkdir(path.c_str(), 0755) == 0)
    return true;
  if (errno == EEXIST) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
  }
  PERFETTO_ELOG("mkdir(%s) failed: %s", path.c_str(), strerror(errno));
  return false;
}

bool WriteWholeFile(const std::string& path, const void* data, size_t size) {
  base::ScopedFile fd(base::OpenFile(path, O_WRONLY | O_CREAT | O_TRUNC, 0644));
  if (!fd) {
    PERFETTO_ELOG("Cannot open %s: %s", path.c_str(), strerror(errno));
    return false;
  }
  return base::WriteAll(fd.get(), data, size) == static_cast<ssize_t>(size);
}

// Returns the absolute path to argv[0], resolving "./" etc. so we can spawn
// ourselves as a producer worker.
std::string ResolveSelfExe() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return std::string(buf);
  }
  return std::string("trace_replay");
}

// Returns the dir component of `path`, or "." if there's no slash.
std::string DirName(const std::string& path) {
  auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Tries to connect() to the given UNIX socket path. Returns true on success.
bool CanConnectToUnixSocket(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    close(fd);
    return false;
  }
  memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  bool ok =
      connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
  close(fd);
  return ok;
}

// Reads /proc/PID/comm for every PID dir; returns the first PID whose comm
// matches `comm`. -1 if not found.
int FindProcessByComm(const std::string& comm) {
  DIR* d = opendir("/proc");
  if (!d)
    return -1;
  int found = -1;
  while (struct dirent* ent = readdir(d)) {
    if (ent->d_type != DT_DIR)
      continue;
    int pid = atoi(ent->d_name);
    if (pid <= 0)
      continue;
    std::string p = std::string("/proc/") + ent->d_name + "/comm";
    std::string content;
    if (!base::ReadFile(p, &content))
      continue;
    // /proc/<pid>/comm has a trailing newline.
    while (!content.empty() && (content.back() == '\n' || content.back() == 0))
      content.pop_back();
    if (content == comm) {
      found = pid;
      break;
    }
  }
  closedir(d);
  return found;
}

struct BackendInfo {
  std::string producer_sock;
  std::string consumer_sock;
  int traced_pid = -1;
  std::unique_ptr<base::Subprocess> tracebox;  // if we spawned it
};

struct IterationMetrics {
  std::string iter_dir;
  double wall_ms = 0;
  double traced_cpu_user_ms = 0;
  double traced_cpu_sys_ms = 0;
  long traced_rss_peak_kb = 0;
  size_t out_trace_bytes = 0;
};

void PrintSingleRunSummary(const IterationMetrics& m) {
  fprintf(stderr,
          "\n------------- trace_replay summary -------------\n"
          "  wall                 : %10.1f ms\n"
          "  traced cpu (user)    : %10.1f ms\n"
          "  traced cpu (sys)     : %10.1f ms\n"
          "  traced cpu (total)   : %10.1f ms\n"
          "  traced rss peak      : %10ld kB\n"
          "  output trace size    : %10zu B\n"
          "------------------------------------------------\n",
          m.wall_ms, m.traced_cpu_user_ms, m.traced_cpu_sys_ms,
          m.traced_cpu_user_ms + m.traced_cpu_sys_ms, m.traced_rss_peak_kb,
          m.out_trace_bytes);
}

struct Stats {
  double mean = 0, median = 0, stddev = 0, min = 0, max = 0;
};

template <typename Getter>
Stats ComputeStats(const std::vector<IterationMetrics>& xs, Getter g) {
  std::vector<double> vs;
  vs.reserve(xs.size());
  for (const auto& x : xs)
    vs.push_back(g(x));
  std::sort(vs.begin(), vs.end());
  Stats s;
  if (vs.empty())
    return s;
  s.min = vs.front();
  s.max = vs.back();
  s.median = vs[vs.size() / 2];
  double sum = 0;
  for (double v : vs)
    sum += v;
  s.mean = sum / static_cast<double>(vs.size());
  if (vs.size() > 1) {
    double sq = 0;
    for (double v : vs) {
      double d = v - s.mean;
      sq += d * d;
    }
    s.stddev =
        std::sqrt(sq / static_cast<double>(vs.size() - 1));  // sample stddev
  }
  return s;
}

void PrintBenchmarkSummary(const std::vector<IterationMetrics>& ms) {
  const Stats wall =
      ComputeStats(ms, [](const IterationMetrics& m) { return m.wall_ms; });
  const Stats cpu = ComputeStats(ms, [](const IterationMetrics& m) {
    return m.traced_cpu_user_ms + m.traced_cpu_sys_ms;
  });
  const Stats rss = ComputeStats(ms, [](const IterationMetrics& m) {
    return static_cast<double>(m.traced_rss_peak_kb);
  });
  const Stats out_sz = ComputeStats(ms, [](const IterationMetrics& m) {
    return static_cast<double>(m.out_trace_bytes);
  });

  fprintf(stderr,
          "\n========================= trace_replay benchmark "
          "=========================\n");
  fprintf(stderr, "Iterations: %zu\n\n", ms.size());
  fprintf(stderr, "%-22s %12s %12s %12s %12s %12s\n", "Metric", "mean",
          "median", "stddev", "min", "max");
  fprintf(stderr,
          "------------------------------------------------------------------"
          "----------\n");
  fprintf(stderr, "%-22s %12.1f %12.1f %12.1f %12.1f %12.1f\n", "wall (ms)",
          wall.mean, wall.median, wall.stddev, wall.min, wall.max);
  fprintf(stderr, "%-22s %12.1f %12.1f %12.1f %12.1f %12.1f\n",
          "traced cpu (ms)", cpu.mean, cpu.median, cpu.stddev, cpu.min,
          cpu.max);
  fprintf(stderr, "%-22s %12.0f %12.0f %12.1f %12.0f %12.0f\n",
          "traced rss peak (kB)", rss.mean, rss.median, rss.stddev, rss.min,
          rss.max);
  fprintf(stderr, "%-22s %12.0f %12.0f %12.1f %12.0f %12.0f\n",
          "output trace (bytes)", out_sz.mean, out_sz.median, out_sz.stddev,
          out_sz.min, out_sz.max);
  fprintf(stderr,
          "==================================================================="
          "=========\n\n");
}

bool SetupBackend(const OrchestratorOptions& opts,
                  const std::string& out_dir,
                  BackendInfo* info) {
  if (!opts.use_tracebox) {
    // Try system traced first.
    const char* def_prod = getenv("PERFETTO_PRODUCER_SOCK_NAME");
    if (!def_prod)
      def_prod =
#if defined(__ANDROID__)
          "/dev/socket/traced_producer";
#else
          "/tmp/perfetto-producer";
#endif
    if (CanConnectToUnixSocket(def_prod)) {
      info->producer_sock = def_prod;
      const char* def_cons = getenv("PERFETTO_CONSUMER_SOCK_NAME");
      if (!def_cons)
        def_cons =
#if defined(__ANDROID__)
            "/dev/socket/traced_consumer";
#else
            "/tmp/perfetto-consumer";
#endif
      info->consumer_sock = def_cons;
      info->traced_pid = FindProcessByComm("traced");
      if (info->traced_pid < 0) {
        PERFETTO_ELOG(
            "Connected to system producer socket but could not find a "
            "process named 'traced' in /proc. Monitoring will be skipped.");
      }
      PERFETTO_LOG("Using system traced (pid=%d, sock=%s)", info->traced_pid,
                   def_prod);
      return true;
    }
    PERFETTO_LOG(
        "System producer socket %s unreachable; falling back to tracebox.",
        def_prod);
  }

  // tracebox path.
  std::string sock_dir = out_dir + "/socks";
  if (!MakeDirP(sock_dir))
    return false;
  info->producer_sock = sock_dir + "/producer";
  info->consumer_sock = sock_dir + "/consumer";

  std::string self = ResolveSelfExe();
  std::string outbin = DirName(self);
  std::string tracebox_path = outbin + "/tracebox";
  struct stat st{};
  if (stat(tracebox_path.c_str(), &st) != 0) {
    PERFETTO_ELOG(
        "tracebox not found next to %s (expected %s). Build it with "
        "`tools/ninja -C %s tracebox`.",
        self.c_str(), tracebox_path.c_str(), outbin.c_str());
    return false;
  }

  info->tracebox.reset(new base::Subprocess({tracebox_path, "traced"}));
  info->tracebox->args.env = {
      "PERFETTO_PRODUCER_SOCK_NAME=" + info->producer_sock,
      "PERFETTO_CONSUMER_SOCK_NAME=" + info->consumer_sock,
  };
  info->tracebox->args.stdout_mode = base::Subprocess::OutputMode::kInherit;
  info->tracebox->args.stderr_mode = base::Subprocess::OutputMode::kInherit;
  info->tracebox->Start();
  info->traced_pid = info->tracebox->pid();
  PERFETTO_LOG("Spawned tracebox traced (pid=%d, prod=%s, cons=%s)",
               info->traced_pid, info->producer_sock.c_str(),
               info->consumer_sock.c_str());

  // Wait until producer socket becomes reachable (up to 5s).
  for (int i = 0; i < 50; i++) {
    if (CanConnectToUnixSocket(info->producer_sock))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  PERFETTO_ELOG("tracebox traced did not come up within 5s");
  return false;
}

}  // namespace

int RunOrchestrator(const OrchestratorOptions& opts) {
  if (opts.input_trace_path.empty()) {
    PERFETTO_ELOG("No input trace path");
    return 1;
  }

  std::string out_dir = opts.out_dir.empty() ? DefaultOutDir() : opts.out_dir;
  if (!MakeDirP(out_dir))
    return 1;

  PERFETTO_LOG("Analyzing %s ...", opts.input_trace_path.c_str());
  TraceAnalysis ana;
  AnalyzeOptions ao;
  ao.ignore_orphan_writers = opts.ignore_orphan_writers;
  ao.zero_delay = opts.zero_delay;
  auto status = AnalyzeTraceFile(opts.input_trace_path, ao, &ana);
  if (!status.ok()) {
    PERFETTO_ELOG("Analysis failed: %s", status.c_message());
    return 1;
  }

  std::set<uint32_t> used_buffers;
  for (const auto& kv : ana.records_by_pid) {
    for (const auto& r : kv.second)
      used_buffers.insert(r.buffer_idx);
  }

  if (used_buffers.empty()) {
    PERFETTO_ELOG("No replayable packets found in the input trace");
    return 1;
  }

  uint32_t max_buf_idx = *used_buffers.rbegin();
  if (max_buf_idx + 1 > opts.max_buffers) {
    PERFETTO_ELOG("Input trace uses buffer index %u, exceeds --max-buffers=%u",
                  max_buf_idx, opts.max_buffers);
    return 1;
  }
  if (max_buf_idx + 1 > 32) {
    PERFETTO_ELOG(
        "Replay supports at most 32 buffers (compile-time ReplayDS<0..31>). "
        "Trace uses buf=%u.",
        max_buf_idx);
    return 1;
  }

  uint32_t num_buffers =
      static_cast<uint32_t>(ana.original_config.buffers().size());

  PERFETTO_LOG("Trace summary: pids=%zu  total_packets=%" PRIu64
               "  buffers_used=%zu  "
               "(of %u in cfg)  min_ts=%" PRIu64 "  max_rel_ts_ms=%" PRIu64
               "  skipped_service=%" PRIu64 "  skipped_no_pid=%" PRIu64,
               ana.records_by_pid.size(), ana.total_packets,
               used_buffers.size(), num_buffers, ana.min_ts_ns,
               ana.max_rel_ts_ns / 1000000, ana.skipped_service_packets,
               ana.skipped_no_pid_packets);
  PERFETTO_LOG("Packet mapping: by_stats=%" PRIu64 "  by_content=%" PRIu64
               "  defaulted_to_buf0=%" PRIu64 "  dropped_orphan=%" PRIu64,
               ana.mapping_stats.packets_resolved_by_stats,
               ana.mapping_stats.packets_resolved_by_content,
               ana.mapping_stats.packets_defaulted_to_buf0,
               ana.mapping_stats.packets_dropped_orphan);
  for (const auto& kv : ana.records_by_pid) {
    std::set<uint32_t> seqs, bufs;
    for (const auto& r : kv.second) {
      seqs.insert(r.orig_seq_id);
      bufs.insert(r.buffer_idx);
    }
    PERFETTO_LOG("  pid=%d  packets=%zu  seqs=%zu  bufs=%zu", kv.first,
                 kv.second.size(), seqs.size(), bufs.size());
  }

  // Forge config.
  ForgeOptions fopts;
  fopts.use_trace_buffer_v2 = opts.use_trace_buffer_v2;
  protos::gen::TraceConfig forged = ForgeReplayConfig(
      ana.original_config, used_buffers, ana.max_rel_ts_ns, fopts);
  std::string forged_pb = forged.SerializeAsString();
  std::string forged_pb_path = out_dir + "/forged.cfg.pb";
  if (!WriteWholeFile(forged_pb_path, forged_pb.data(), forged_pb.size())) {
    PERFETTO_ELOG("Failed to write %s", forged_pb_path.c_str());
    return 1;
  }
  PERFETTO_LOG("Forged TraceConfig: %s (%zu bytes, duration_ms=%u, %d ds)",
               forged_pb_path.c_str(), forged_pb.size(), forged.duration_ms(),
               static_cast<int>(forged.data_sources().size()));

  // Write one replay file per pid.
  struct PidReplay {
    int32_t pid;
    std::string path;
    size_t records;
  };
  std::vector<PidReplay> pid_replays;
  for (auto& kv : ana.records_by_pid) {
    char path[256];
    snprintf(path, sizeof(path), "%s/replay-pid%d.bin", out_dir.c_str(),
             kv.first);
    auto st = WriteReplayFile(path, num_buffers, kv.second);
    if (!st.ok()) {
      PERFETTO_ELOG("Failed to write %s: %s", path, st.c_message());
      return 1;
    }
    PERFETTO_LOG("Wrote %s (records=%zu)", path, kv.second.size());
    pid_replays.push_back({kv.first, path, kv.second.size()});
  }

  if (opts.analyze_only) {
    PERFETTO_LOG("Analyze-only mode requested; stopping after analysis.");
    return 0;
  }

  // -------- live replay --------

  BackendInfo backend;
  if (!SetupBackend(opts, out_dir, &backend))
    return 1;

  std::string self = ResolveSelfExe();
  std::string outbin = DirName(self);
  std::string perfetto_bin = outbin + "/perfetto";
  {
    struct stat st{};
    if (stat(perfetto_bin.c_str(), &st) != 0) {
      PERFETTO_LOG("%s not found; assuming `perfetto` is on PATH.",
                   perfetto_bin.c_str());
      perfetto_bin = "perfetto";
    }
  }

  std::vector<IterationMetrics> metrics;
  metrics.reserve(opts.iterations);
  int rc = 0;

  const long ticks_per_sec = sysconf(_SC_CLK_TCK);

  for (uint32_t iter = 0; iter < opts.iterations; iter++) {
    std::string iter_dir = out_dir;
    if (opts.iterations > 1) {
      char sub[64];
      snprintf(sub, sizeof(sub), "/iter-%u", iter);
      iter_dir += sub;
      if (!MakeDirP(iter_dir)) {
        rc = 1;
        break;
      }
      PERFETTO_LOG("=== iteration %u/%u in %s ===", iter + 1, opts.iterations,
                   iter_dir.c_str());
    }

    IterationMetrics m;
    m.iter_dir = iter_dir;

    // Sample traced's CPU counters at the start of this iteration.
    base::ProcStat ps_start{};
    bool have_ps_start = ReadTracedStats(backend.traced_pid, &ps_start);

    // Spawn producer subprocesses, each with a ready pipe.
    std::vector<std::unique_ptr<base::Subprocess>> producers;
    std::vector<base::Pipe> ready_pipes;
    producers.reserve(pid_replays.size());
    ready_pipes.reserve(pid_replays.size());
    for (const auto& pr : pid_replays) {
      base::Pipe pipe = base::Pipe::Create();
      int write_fd = pipe.wr.get();
      auto sub = std::make_unique<base::Subprocess>();
      sub->args.exec_cmd = {self, "--replay-worker", pr.path, "--ready-fd",
                            std::to_string(write_fd)};
      sub->args.env = {
          "PERFETTO_PRODUCER_SOCK_NAME=" + backend.producer_sock,
          "PERFETTO_CONSUMER_SOCK_NAME=" + backend.consumer_sock,
      };
      sub->args.preserve_fds.push_back(write_fd);
      sub->args.stdout_mode = base::Subprocess::OutputMode::kInherit;
      sub->args.stderr_mode = base::Subprocess::OutputMode::kInherit;
      sub->Start();
      pipe.wr.reset();
      producers.push_back(std::move(sub));
      ready_pipes.push_back(std::move(pipe));
    }

    for (size_t i = 0; i < ready_pipes.size(); i++) {
      char ch = 0;
      ssize_t n = read(ready_pipes[i].rd.get(), &ch, 1);
      if (n != 1 || ch != 'R') {
        PERFETTO_ELOG("Producer pid=%d did not signal ready (read=%zd)",
                      producers[i]->pid(), n);
        rc = 1;
        break;
      }
    }
    if (rc != 0)
      break;
    PERFETTO_LOG("All %zu producers ready.", producers.size());

    // Per-iteration monitor and perf.
    std::unique_ptr<ProcMonitor> monitor;
    if (backend.traced_pid > 0) {
      monitor.reset(new ProcMonitor(backend.traced_pid,
                                    iter_dir + "/monitor.csv",
                                    opts.monitor_interval_ms));
      monitor->Start();
    }
    // perf record (sampling + callstacks) or perf stat (counters only).
    // We sample on `task-clock` rather than `cycles` so this works in
    // environments without a hardware PMU (containers, VMs).
    std::unique_ptr<base::Subprocess> perf;
    if (opts.capture_perf && backend.traced_pid > 0) {
      perf.reset(new base::Subprocess(
          {"perf", "record", "-e", "task-clock", "-F", "4999", "-g", "-p",
           std::to_string(backend.traced_pid), "-o",
           iter_dir + "/perf.data"}));
      perf->args.stdout_mode = base::Subprocess::OutputMode::kInherit;
      perf->args.stderr_mode = base::Subprocess::OutputMode::kInherit;
      perf->Start();
    }
    std::unique_ptr<base::Subprocess> perf_stat;
    if (opts.capture_perf_stat && backend.traced_pid > 0) {
      perf_stat.reset(new base::Subprocess(
          {"perf", "stat", "-e",
           "task-clock,context-switches,cpu-migrations,page-faults,"
           "minor-faults,major-faults,cycles,instructions,branch-misses,"
           "cache-references,cache-misses",
           "-p", std::to_string(backend.traced_pid), "-o",
           iter_dir + "/perf.stat.txt"}));
      perf_stat->args.stdout_mode = base::Subprocess::OutputMode::kInherit;
      perf_stat->args.stderr_mode = base::Subprocess::OutputMode::kInherit;
      perf_stat->Start();
    }

    // Launch perfetto consumer.
    std::string out_trace = iter_dir + "/out.trace";
    base::Subprocess perfetto_proc(
        {perfetto_bin, "-c", forged_pb_path, "-o", out_trace});
    perfetto_proc.args.env = {
        "PERFETTO_PRODUCER_SOCK_NAME=" + backend.producer_sock,
        "PERFETTO_CONSUMER_SOCK_NAME=" + backend.consumer_sock,
    };
    perfetto_proc.args.stdout_mode = base::Subprocess::OutputMode::kInherit;
    perfetto_proc.args.stderr_mode = base::Subprocess::OutputMode::kInherit;
    perfetto_proc.Start();
    PERFETTO_LOG("Launched perfetto cmdline (pid=%d, out=%s)",
                 perfetto_proc.pid(), out_trace.c_str());

    const int64_t t_start_ms = base::GetWallTimeMs().count();
    const int64_t total_ms =
        static_cast<int64_t>(ana.max_rel_ts_ns / 1000000ull);
    int64_t last_progress_ms = t_start_ms;

    for (;;) {
      size_t alive = 0;
      for (auto& sub : producers) {
        if (sub->Poll() != base::Subprocess::kTerminated)
          alive++;
      }
      int64_t now_ms = base::GetWallTimeMs().count();
      if (now_ms - last_progress_ms >= 10000 || alive == 0) {
        int64_t elapsed_ms = now_ms - t_start_ms;
        int64_t remaining_ms =
            total_ms > elapsed_ms ? total_ms - elapsed_ms : 0;
        long rss_kb = ReadTracedRssKb(backend.traced_pid);
        char rss_str[32];
        if (rss_kb >= 0)
          snprintf(rss_str, sizeof(rss_str), "%ld kB", rss_kb);
        else
          snprintf(rss_str, sizeof(rss_str), "n/a");
        if (total_ms > 0) {
          double pct = std::min(100.0, 100.0 * static_cast<double>(elapsed_ms) /
                                           static_cast<double>(total_ms));
          PERFETTO_LOG("replay progress: %.1f%% (%" PRId64 " / %" PRId64
                       " ms)  ETA %" PRId64
                       "s  producers_alive=%zu/%zu  traced_rss=%s",
                       pct, elapsed_ms, total_ms, remaining_ms / 1000, alive,
                       producers.size(), rss_str);
        } else {
          PERFETTO_LOG("replay progress: elapsed=%" PRId64
                       " ms  producers_alive=%zu/%zu  traced_rss=%s",
                       elapsed_ms, alive, producers.size(), rss_str);
        }
        last_progress_ms = now_ms;
      }
      if (alive == 0)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Producers done. Sample traced CPU now, before SIGINT'ing perfetto
    // (we want to attribute the replay work itself, not the buffer drain).
    base::ProcStat ps_end{};
    bool have_ps_end = ReadTracedStats(backend.traced_pid, &ps_end);

    m.wall_ms = static_cast<double>(base::GetWallTimeMs().count() - t_start_ms);
    if (have_ps_start && have_ps_end && ticks_per_sec > 0) {
      double utime = static_cast<double>(ps_end.utime - ps_start.utime);
      double stime = static_cast<double>(ps_end.stime - ps_start.stime);
      m.traced_cpu_user_ms =
          utime * 1000.0 / static_cast<double>(ticks_per_sec);
      m.traced_cpu_sys_ms = stime * 1000.0 / static_cast<double>(ticks_per_sec);
    }
    m.traced_rss_peak_kb = monitor ? monitor->peak_rss_kb() : 0;

    PERFETTO_LOG(
        "iter %u done: wall=%.0f ms  traced_cpu=%.0f ms (user=%.0f + sys=%.0f)"
        "  traced_rss_peak=%ld kB",
        iter + 1, m.wall_ms, m.traced_cpu_user_ms + m.traced_cpu_sys_ms,
        m.traced_cpu_user_ms, m.traced_cpu_sys_ms, m.traced_rss_peak_kb);

    // Stop the consumer and ancillary processes.
    perfetto_proc.Kill(SIGINT);
    if (!perfetto_proc.Wait(30000))
      perfetto_proc.KillAndWaitForTermination(SIGTERM);
    if (perf) {
      perf->Kill(SIGINT);
      perf->Wait(10000);
    }
    if (perf_stat) {
      perf_stat->Kill(SIGINT);
      perf_stat->Wait(10000);
    }
    if (monitor)
      monitor->Stop();

    struct stat ot{};
    if (stat(out_trace.c_str(), &ot) == 0)
      m.out_trace_bytes = static_cast<size_t>(ot.st_size);
    metrics.push_back(m);
  }

  if (backend.tracebox)
    backend.tracebox->KillAndWaitForTermination(SIGTERM);

  // ----- benchmark summary -----
  if (!metrics.empty() && opts.iterations > 1)
    PrintBenchmarkSummary(metrics);
  else if (!metrics.empty())
    PrintSingleRunSummary(metrics.front());

  PERFETTO_LOG("Done. Artifacts in %s", out_dir.c_str());
  return rc;
}

}  // namespace trace_replay
}  // namespace perfetto
