/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_NINJA_NINJA_LOG_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_NINJA_NINJA_LOG_PARSER_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"

namespace perfetto::trace_processor {

class TraceImporterBase;
class TraceProcessorContext;

// Creates the importer for the ninja build log trace type (.ninja_log).
std::unique_ptr<TraceImporterBase> CreateNinjaLogImporter();

// This class parses Ninja's (the build system, ninja-build.org) build logs and
// turns them into traces. A ninja log typically contains the logs of >1 ninja
// invocation. We map those as follows:
// - All invocations share one synthesized process, but their timestamps do not
//   overlap: each invocation is shifted past the end of the previous one, so
//   the trace reads as the build history of that output directory.
// - Within each invocation we work out the parallelism from the time stamp and
//   create one thread for each concurrent stream of jobs. Threads are reused
//   across invocations, so the number of tracks stays close to the -j level
//   rather than growing with the number of builds in the log.
// Caveat: this works only if ninja didn't recompact the logs. Once recompaction
// happens (can be forced via ninja -t recompact) there is no way to identify
// the boundaries of each build (recompaction deletes, for each hash, all but
// the most recent timestamp and rewrites the log).
class NinjaLogParser : public ChunkedTraceReader {
 public:
  explicit NinjaLogParser(TraceProcessorContext*);
  ~NinjaLogParser() override;
  NinjaLogParser(const NinjaLogParser&) = delete;
  NinjaLogParser& operator=(const NinjaLogParser&) = delete;

  // ChunkedTraceReader implementation
  base::Status Parse(TraceBlobView) override;
  base::Status OnPushDataToSorter() override;
  void OnEventsFullyExtracted() override {}

 private:
  struct Job {
    Job(int64_t s, int64_t e, uint64_t h, const std::string& n)
        : start_ms(s), end_ms(e), hash(h), names(n) {}

    int64_t start_ms;
    int64_t end_ms;
    uint64_t hash;  // Hash of the compiler invocation cmdline.

    // Typically the one output for the compiler invocation. In case of actions
    // generating multiple outputs this contains the join of all output names.
    std::string names;
  };

  TraceProcessorContext* const ctx_;
  bool header_parsed_ = false;

  // The end timestamp of the last job seen, used to detect the boundary
  // between two ninja invocations (see the note on |build_offset_ms_|).
  int64_t last_end_seen_ = 0;

  // Timestamps in a ninja log restart from zero on every invocation, so the
  // jobs of two builds would otherwise be drawn on top of each other. Each
  // build after the first is shifted by the sum of the durations of the
  // builds before it, which keeps every job in the trace without inventing
  // parallelism that never happened.
  int64_t build_offset_ms_ = 0;

  std::vector<Job> jobs_;
  std::vector<char> log_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_NINJA_NINJA_LOG_PARSER_H_
