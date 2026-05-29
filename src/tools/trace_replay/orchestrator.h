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

#ifndef SRC_TOOLS_TRACE_REPLAY_ORCHESTRATOR_H_
#define SRC_TOOLS_TRACE_REPLAY_ORCHESTRATOR_H_

#include <cstdint>
#include <string>

namespace perfetto {
namespace trace_replay {

struct OrchestratorOptions {
  std::string input_trace_path;
  std::string out_dir;
  bool analyze_only = false;
  bool use_tracebox = false;
  bool capture_perf = false;
  uint32_t monitor_interval_ms = 250;
  bool ignore_orphan_writers = false;
  uint32_t max_buffers = 32;

  // Skip real-time pacing entirely: every packet fires back-to-back as fast
  // as the producer can push.
  bool zero_delay = false;

  // How many times to run the replay back-to-back. >1 enables a small
  // benchmark-style summary at the end.
  uint32_t iterations = 1;

  // If true, force every buffer in the forged TraceConfig to use the
  // experimental TRACE_BUFFER_V2 implementation
  // (BufferConfig.experimental_mode = TRACE_BUFFER_V2).
  bool use_trace_buffer_v2 = false;
};

int RunOrchestrator(const OrchestratorOptions& opts);

}  // namespace trace_replay
}  // namespace perfetto

#endif  // SRC_TOOLS_TRACE_REPLAY_ORCHESTRATOR_H_
