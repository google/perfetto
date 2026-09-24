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
  }

  return redactor->Redact(input, output, &context);
}

}  // namespace perfetto::trace_redaction

int main(int argc, char** argv) {
  constexpr int kSuccess = 0;
  constexpr int kFailure = 1;
  constexpr int kInvalidArgs = 2;

  if (argc != 4 && argc != 5) {
    PERFETTO_ELOG(
        "Usage: %s <input file> <output file> <package name> [target uid]",
        argv[0]);
    return kInvalidArgs;
  }

  std::optional<uint64_t> target_uid;
  if (argc == 5) {
    const char* uid_str = argv[4];
    if (uid_str[0] == '-' || uid_str[0] == '\0') {
      PERFETTO_ELOG("Invalid target uid (cannot be negative or empty): %s",
                    uid_str);
      return kInvalidArgs;
    }
    auto parsed = perfetto::base::CStringToUInt64(uid_str);
    if (!parsed.has_value() ||
        *parsed == perfetto::trace_redaction::ProcessThreadTimeline::Event::
                       kUnknownUid ||
        *parsed > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      PERFETTO_ELOG("Invalid target uid: %s", uid_str);
      return kInvalidArgs;
    }
    target_uid = *parsed;
  }

  auto result =
      perfetto::trace_redaction::Main(argv[1], argv[2], argv[3], target_uid);

  if (result.ok()) {
    return kSuccess;
  }

  PERFETTO_ELOG("Unexpected error: %s", result.c_message());
  return kFailure;
}
