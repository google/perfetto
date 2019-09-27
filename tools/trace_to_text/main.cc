/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

#include "perfetto/base/logging.h"
#include "tools/trace_to_text/symbolize_profile.h"
#include "tools/trace_to_text/trace_to_profile.h"
#include "tools/trace_to_text/trace_to_systrace.h"
#include "tools/trace_to_text/trace_to_text.h"

#if PERFETTO_BUILDFLAG(PERFETTO_VERSION_GEN)
#include "perfetto_version.gen.h"
#else
#define PERFETTO_GET_GIT_REVISION() "unknown"
#endif

namespace perfetto {
namespace trace_to_text {
namespace {

int Usage(const char* argv0) {
  printf(
      "Usage: %s systrace|json|ctrace|text|profile [--truncate start|end] "
      "[trace.pb] "
      "[trace.txt]\n",
      argv0);
  return 1;
}

int Main(int argc, char** argv) {
  std::vector<const char*> positional_args;
  Keep truncate_keep = Keep::kAll;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      printf("%s\n", PERFETTO_GET_GIT_REVISION());
      return 0;
    } else if (strcmp(argv[i], "-t") == 0 ||
               strcmp(argv[i], "--truncate") == 0) {
      i++;
      if (i <= argc && strcmp(argv[i], "start") == 0) {
        truncate_keep = Keep::kStart;
      } else if (i <= argc && strcmp(argv[i], "end") == 0) {
        truncate_keep = Keep::kEnd;
      } else {
        PERFETTO_ELOG(
            "--truncate must specify whether to keep the end or the "
            "start of the trace.");
        return Usage(argv[0]);
      }
    } else {
      positional_args.push_back(argv[i]);
    }
  }

  if (positional_args.size() < 1)
    return Usage(argv[0]);

  std::istream* input_stream;
  std::ifstream file_istream;
  if (positional_args.size() > 1) {
    const char* file_path = positional_args[1];
    file_istream.open(file_path, std::ios_base::in | std::ios_base::binary);
    if (!file_istream.is_open())
      PERFETTO_FATAL("Could not open %s", file_path);
    input_stream = &file_istream;
  } else {
    if (isatty(STDIN_FILENO)) {
      PERFETTO_ELOG("Reading from stdin but it's connected to a TTY");
      PERFETTO_LOG("It is unlikely that you want to type in some binary.");
      PERFETTO_LOG("Either pass a file path to the cmdline or pipe stdin");
      return Usage(argv[0]);
    }
    input_stream = &std::cin;
  }

  std::ostream* output_stream;
  std::ofstream file_ostream;
  if (positional_args.size() > 2) {
    const char* file_path = positional_args[2];
    file_ostream.open(file_path, std::ios_base::out | std::ios_base::trunc);
    if (!file_ostream.is_open())
      PERFETTO_FATAL("Could not open %s", file_path);
    output_stream = &file_ostream;
  } else {
    output_stream = &std::cout;
  }

  std::string format(positional_args[0]);

  if (format == "json")
    return TraceToSystrace(input_stream, output_stream, kSystraceJson,
                           truncate_keep);

  if (format == "systrace")
    return TraceToSystrace(input_stream, output_stream, kSystraceNormal,
                           truncate_keep);

  if (format == "ctrace")
    return TraceToSystrace(input_stream, output_stream, kSystraceCompressed,
                           truncate_keep);

  if (truncate_keep != Keep::kAll) {
    PERFETTO_ELOG(
        "--truncate is unsupported for text|profile|symbolize format.");
    return 1;
  }

  if (format == "text")
    return TraceToText(input_stream, output_stream);

  if (format == "profile")
    return TraceToProfile(input_stream, output_stream);

  if (format == "symbolize")
    return SymbolizeProfile(input_stream, output_stream);

  return Usage(argv[0]);
}

}  // namespace
}  // namespace trace_to_text
}  // namespace perfetto

int main(int argc, char** argv) {
  return perfetto::trace_to_text::Main(argc, argv);
}
