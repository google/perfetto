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

#include <limits>
#include <optional>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/trace_redaction/trace_redaction_framework.h"
#include "src/trace_redaction/trace_redactor.h"
#include "src/trace_redaction/verify_integrity.h"

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

// Holds optional command-line arguments parsed for trace_redactor.
struct Options {
  // Explicit Linux UID of the target package instance (--uid <target uid>).
  // When unset, trace_redactor resolves the package's base App ID from the
  // trace's PackagesList and normalizes process UIDs across user profiles.
  std::optional<uint64_t> target_uid;
};

static bool ParseUidOption(const std::string& uid_str, Options* options) {
  if (options->target_uid.has_value()) {
    PERFETTO_ELOG("Duplicate --uid argument");
    return false;
  }
  if (uid_str.empty() || uid_str.front() == '-') {
    PERFETTO_ELOG("Invalid target uid (cannot be negative or empty): %s",
                  uid_str.c_str());
    return false;
  }
  auto parsed = base::StringToUInt64(uid_str);
  if (!parsed.has_value() ||
      *parsed == ProcessThreadTimeline::Event::kUnknownUid ||
      *parsed > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
    PERFETTO_ELOG("Invalid target uid: %s", uid_str.c_str());
    return false;
  }
  options->target_uid = *parsed;
  return true;
}

static bool ParseOptionalArgs(int argc,
                              char** argv,
                              int start_index,
                              Options* options) {
  for (int i = start_index; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--uid") {
      if (i + 1 >= argc) {
        PERFETTO_ELOG("Missing argument for %s", arg.c_str());
        return false;
      }
      std::string value(argv[++i]);
      if (!ParseUidOption(value, options)) {
        return false;
      }
    } else {
      // Future optional arguments (e.g. `else if (arg == "--...")`) can be
      // parsed here.
      PERFETTO_ELOG("Unknown argument: %s", arg.c_str());
      return false;
    }
  }
  return true;
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

  perfetto::trace_redaction::Options options;
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
