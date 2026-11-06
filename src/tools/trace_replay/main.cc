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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/getopt.h"
#include "perfetto/ext/base/string_utils.h"

#include "src/tools/trace_replay/orchestrator.h"
#include "src/tools/trace_replay/producer_worker.h"

namespace perfetto {
namespace trace_replay {
namespace {

void PrintUsage(const char* argv0) {
  fprintf(
      stderr,
      "Usage: %s [options] <input.trace>\n"
      "\n"
      "Replay a Perfetto trace against the running traced (or tracebox)\n"
      "in order to assess service-side performance.\n"
      "\n"
      "Options:\n"
      "  --out-dir DIR             Output directory (artifacts go here).\n"
      "                            Default: a fresh /tmp/replay.XXXXXX.\n"
      "  --iterations N            Run the replay N times (default 1) and\n"
      "                            print a benchmark-style summary.\n"
      "  --use-trace-buffer-v2     Force every buffer in the forged config\n"
      "                            to BufferConfig.experimental_mode =\n"
      "                            TRACE_BUFFER_V2.\n"
      "  --use-tracebox            Spawn our own traced via tracebox\n"
      "                            instead of using the system one.\n"
      "  --perf                    Run `perf record -g` on traced (callstack\n"
      "                            sampling).\n"
      "  --perf-stat               Run `perf stat` on traced (counters only,\n"
      "                            no callstacks). Cheaper than --perf.\n"
      "  --monitor-interval-ms N   /proc poll interval (default 250).\n"
      "  --ignore-orphan-writers   Drop packets whose sequence_id is\n"
      "                            missing from trace_stats.writer_stats.\n"
      "  --max-buffers N           Refuse traces with more buffers than\n"
      "                            this (default 32, hard cap 32).\n"
      "  --analyze-only            Run analysis + config forge, no replay.\n"
      "  --zero-delay              Skip real-time pacing entirely; fire\n"
      "                            every packet ASAP. Required when the\n"
      "                            trace contains non-default clocks.\n"
      "\n"
      "Internal:\n"
      "  --replay-worker FILE --ready-fd N\n"
      "                            Invoked by the orchestrator in child\n"
      "                            processes; not for human use.\n",
      argv0);
}

}  // namespace
}  // namespace trace_replay
}  // namespace perfetto

int main(int argc, char** argv) {
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) &&   \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  fprintf(stderr, "trace_replay is supported only on Linux/Android/macOS.\n");
  return 1;
#else
  using namespace perfetto::trace_replay;

  static const struct option kLongOpts[] = {
      {"out-dir", required_argument, nullptr, 1001},
      {"use-tracebox", no_argument, nullptr, 1002},
      {"perf", no_argument, nullptr, 1003},
      {"monitor-interval-ms", required_argument, nullptr, 1004},
      {"ignore-orphan-writers", no_argument, nullptr, 1005},
      {"max-buffers", required_argument, nullptr, 1006},
      {"analyze-only", no_argument, nullptr, 1007},
      {"zero-delay", no_argument, nullptr, 1008},
      {"iterations", required_argument, nullptr, 1009},
      {"use-trace-buffer-v2", no_argument, nullptr, 1010},
      {"perf-stat", no_argument, nullptr, 1011},
      {"replay-worker", required_argument, nullptr, 1100},
      {"ready-fd", required_argument, nullptr, 1101},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  OrchestratorOptions oo;
  ProducerWorkerOptions pw;
  bool is_worker = false;

  for (;;) {
    int c = getopt_long(argc, argv, "h", kLongOpts, nullptr);
    if (c == -1)
      break;
    switch (c) {
      case 1001:
        oo.out_dir = optarg;
        break;
      case 1002:
        oo.use_tracebox = true;
        break;
      case 1003:
        oo.capture_perf = true;
        break;
      case 1004:
        oo.monitor_interval_ms = static_cast<uint32_t>(atoi(optarg));
        break;
      case 1005:
        oo.ignore_orphan_writers = true;
        break;
      case 1006:
        oo.max_buffers = static_cast<uint32_t>(atoi(optarg));
        break;
      case 1007:
        oo.analyze_only = true;
        break;
      case 1008:
        oo.zero_delay = true;
        break;
      case 1009:
        oo.iterations = static_cast<uint32_t>(atoi(optarg));
        if (oo.iterations == 0)
          oo.iterations = 1;
        break;
      case 1010:
        oo.use_trace_buffer_v2 = true;
        break;
      case 1011:
        oo.capture_perf_stat = true;
        break;
      case 1100:
        is_worker = true;
        pw.replay_file = optarg;
        break;
      case 1101:
        pw.ready_fd = atoi(optarg);
        break;
      case 'h':
      default:
        PrintUsage(argv[0]);
        return c == 'h' ? 0 : 1;
    }
  }

  if (is_worker) {
    if (pw.replay_file.empty()) {
      fprintf(stderr, "--replay-worker requires a file argument.\n");
      return 1;
    }
    return RunProducerWorker(pw);
  }

  if (optind >= argc) {
    PrintUsage(argv[0]);
    return 1;
  }
  oo.input_trace_path = argv[optind];

  return RunOrchestrator(oo);
#endif
}
