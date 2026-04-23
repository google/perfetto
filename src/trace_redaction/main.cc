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

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
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
                         int nice) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) && \
    PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  // This will only occur for builds from android repository.
  setpriority(PRIO_PROCESS, 0, nice);
#else
  base::ignore_result(nice);
#endif

  TraceRedactor::Config config;
  auto redactor = TraceRedactor::CreateInstance(config);

  Context context;
  context.package_name = package_name;

  return redactor->Redact(input, output, &context);
}

static int Usage(const char* argv0) {
  fprintf(stderr, R"(
Trace redaction tool.
Usage: %s [OPTIONS] <input_file> <output_file> <package_name>

OPTIONS:
    --nice <nice_value>                 Setup nice value for the redaction process. (Android-only)
)",
          argv0);
  return 1;
}

}  // namespace perfetto::trace_redaction

int main(int argc, char** argv) {
  constexpr int kSuccess = 0;
  constexpr int kFailure = 1;
  constexpr int kInvalidArgs = 2;

  std::vector<const char*> positional_args;
  int nice = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--nice") == 0) {
      if (i + 1 < argc) {
        auto nice_opt = perfetto::base::CStringToInt32(argv[++i]);
        if (nice_opt.has_value()) {
          nice = nice_opt.value();
        } else {
          PERFETTO_ELOG("Invalid value for --nice: %s", argv[i]);
          return kInvalidArgs;
        }
      } else {
        PERFETTO_ELOG("--nice requires a value");
        return kInvalidArgs;
      }
    } else {
      positional_args.push_back(argv[i]);
    }
  }

  if (positional_args.size() != 3) {
    PERFETTO_ELOG("Invalid arguments provided to trace_redactor.");
    perfetto::trace_redaction::Usage(argv[0]);
    return kInvalidArgs;
  }

  auto result = perfetto::trace_redaction::Main(
      positional_args[0], positional_args[1], positional_args[2], nice);

  if (result.ok()) {
    return kSuccess;
  }

  PERFETTO_ELOG("Unexpected error: %s", result.c_message());
  return kFailure;
}
