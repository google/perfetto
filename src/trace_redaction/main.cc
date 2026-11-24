/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <cstdint>
#include <optional>
#include <string_view>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"
#include "src/trace_redaction/trace_redactor_options.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
#include <sys/resource.h>
#endif

namespace perfetto::trace_redaction {

// Builds and runs a trace redactor.
static base::Status Main(std::string_view input,
                         std::string_view output,
                         std::string_view package_name,
                         std::optional<uint64_t> target_uid) {
  // Redaction is a low priority task as users are asynchronously informed
  // when redaction finishes so we can keep it as a lower priority for now, if
  // this requirement ever changes, then consider modifying redactor to let
  // caller specify process level thread priority.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  // This will only occur for builds from android repository.
  setpriority(PRIO_PROCESS, 0, 19);
#endif

  TraceRedactor::Config config;
  auto redactor = TraceRedactor::CreateInstance(config);

  Context context;
  context.package_name = package_name;
  if (target_uid.has_value()) {
    context.package_uid = *target_uid;
  } else {
    context.normalize_uid = true;
  }

  return redactor->Redact(input, output, &context);
}

static void PrintUsage(const char* bin_name) {
  PERFETTO_ELOG(
      "Usage: %s <input file> <output file> <package name> "
      "[--uid <target uid>]",
      bin_name);
}

}  // namespace perfetto::trace_redaction

int main(int argc, char** argv) {
  constexpr int kSuccess = 0;
  constexpr int kFailure = 1;
  constexpr int kInvalidArgs = 2;

  if (argc < 4) {
    perfetto::trace_redaction::PrintUsage(argv[0]);
    return kInvalidArgs;
  }

  perfetto::trace_redaction::TraceRedactorOptions options;
  if (!perfetto::trace_redaction::ParseOptionalArgs(argc, argv, 4, &options)) {
    perfetto::trace_redaction::PrintUsage(argv[0]);
    return kInvalidArgs;
  }

  auto result = perfetto::trace_redaction::Main(argv[1], argv[2], argv[3],
                                                options.target_uid);

  if (result.ok()) {
    return kSuccess;
  }

  PERFETTO_ELOG("Unexpected error: %s", result.c_message());
  return kFailure;
}
