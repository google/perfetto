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

#include <cstddef>
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
// - The timestamps of the invocations do not overlap: each invocation is
//   shifted past the end of the previous one, so the trace reads as the build
//   history of that output directory.
// - Within each invocation we work out the parallelism from the time stamp and
//   create one "Worker N" track for each concurrent stream of jobs. Tracks are
//   reused across invocations, so their number stays close to the -j level
//   rather than growing with the number of builds in the log.
// - A "Builds" track has one slice per invocation, and an "Active jobs"
//   counter track has the number of jobs running at each moment.
// Once ninja recompacts the log (on its own, or via ninja -t recompact) the
// boundaries of the builds that were in it are gone: recompaction deletes, for
// each output, all but the most recent entry and rewrites the log. Those
// entries are dropped if builds were appended after them, and shown as a
// single build otherwise.
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

  base::Status ParseLines(size_t size);
  size_t CountRecompactedLines() const;

  TraceProcessorContext* const ctx_;
  bool header_parsed_ = false;

  // One entry per line of the log, in the order and with the timestamps of
  // the log.
  std::vector<Job> jobs_;
  std::vector<char> log_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_NINJA_NINJA_LOG_PARSER_H_
