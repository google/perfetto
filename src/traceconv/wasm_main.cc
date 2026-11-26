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

// Entry point of the UI's traceconv WASM module. The host CLI lives in
// trace_processor_shell, which does not build under Emscripten.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/profiling/pprof_builder.h"
#include "src/traceconv/trace_to_json.h"
#include "src/traceconv/trace_to_profile.h"
#include "src/traceconv/trace_to_systrace.h"

namespace perfetto::traceconv {
namespace {

int Usage(const char* argv0) {
  fprintf(stderr,
          "Usage: %s json|systrace|profile [OPTIONS] input_file "
          "[output_file]\n",
          argv0);
  return 1;
}

int ToExitCode(const base::Status& status) {
  if (status.ok())
    return 0;
  PERFETTO_ELOG("%s", status.c_message());
  return 1;
}

std::optional<uint64_t> ParseUint64(const char* str) {
  std::optional<uint64_t> n = base::CStringToUInt64(str);
  if (!n)
    PERFETTO_ELOG("Invalid %s. Expected decimal integer.", str);
  return n;
}

int Main(int argc, char** argv) {
  std::vector<const char*> positional;
  trace_to_text::Keep keep = trace_to_text::Keep::kAll;
  uint64_t pid = 0;
  std::vector<uint64_t> timestamps;
  std::optional<trace_to_text::ConversionMode> profile_type;
  std::string output_dir;
  for (int i = 1; i < argc; i++) {
    bool has_value = i + 1 < argc;
    if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--truncate") == 0) {
      const char* value = has_value ? argv[++i] : "";
      if (strcmp(value, "start") == 0) {
        keep = trace_to_text::Keep::kStart;
      } else if (strcmp(value, "end") == 0) {
        keep = trace_to_text::Keep::kEnd;
      } else {
        return Usage(argv[0]);
      }
    } else if (strcmp(argv[i], "--pid") == 0 && has_value) {
      std::optional<uint64_t> n = ParseUint64(argv[++i]);
      if (!n)
        return 1;
      pid = *n;
    } else if (strcmp(argv[i], "--timestamps") == 0 && has_value) {
      for (const std::string& ts : base::SplitString(argv[++i], ",")) {
        std::optional<uint64_t> n = ParseUint64(ts.c_str());
        if (!n)
          return 1;
        timestamps.push_back(*n);
      }
    } else if (strcmp(argv[i], "--output-dir") == 0 && has_value) {
      output_dir = argv[++i];
    } else if (strcmp(argv[i], "--alloc") == 0) {
      profile_type = trace_to_text::ConversionMode::kHeapProfile;
    } else if (strcmp(argv[i], "--perf") == 0) {
      profile_type = trace_to_text::ConversionMode::kPerfProfile;
    } else if (strcmp(argv[i], "--java-heap") == 0) {
      profile_type = trace_to_text::ConversionMode::kJavaHeapProfile;
    } else {
      positional.push_back(argv[i]);
    }
  }
  if (positional.size() < 2 || positional.size() > 3)
    return Usage(argv[0]);

  std::string format(positional[0]);
  std::ifstream input(positional[1], std::ios_base::in | std::ios_base::binary);
  if (!input.is_open()) {
    PERFETTO_ELOG("Could not open %s", positional[1]);
    return 1;
  }

  if (format == "profile") {
    if (positional.size() > 2)
      return Usage(argv[0]);
    return ToExitCode(trace_to_text::TraceToProfile(
        &input, pid, timestamps, /*annotate_frames=*/true, output_dir,
        profile_type, /*verbose=*/false, /*quiet=*/false,
        profiling::DebuginfodConfig()));
  }
  if (format != "json" && format != "systrace")
    return Usage(argv[0]);
  if (positional.size() < 3)
    return Usage(argv[0]);
  std::ofstream output(positional[2], std::ios_base::out |
                                          std::ios_base::trunc |
                                          std::ios_base::binary);
  if (!output.is_open()) {
    PERFETTO_ELOG("Could not open %s", positional[2]);
    return 1;
  }
  if (format == "json") {
    return ToExitCode(trace_to_text::TraceToJson(
        &input, &output, /*compress=*/false, keep, /*full_sort=*/false));
  }
  return ToExitCode(trace_to_text::TraceToSystrace(
      &input, &output, /*ctrace=*/false, keep, /*full_sort=*/false));
}

}  // namespace
}  // namespace perfetto::traceconv

int main(int argc, char** argv) {
  return perfetto::traceconv::Main(argc, argv);
}
