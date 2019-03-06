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

#include "perfetto/base/logging.h"
#include "tools/trace_to_text/trace_to_profile.h"
#include "tools/trace_to_text/trace_to_systrace.h"
#include "tools/trace_to_text/trace_to_text.h"

#if PERFETTO_BUILDFLAG(PERFETTO_STANDALONE_BUILD)
#include "perfetto_version.gen.h"
#else
#define PERFETTO_GET_GIT_REVISION() "unknown"
#endif

namespace {

int Usage(const char* argv0) {
  printf(
      "Usage: %s systrace|json|text|profile [trace.pb] "
      "[trace.txt]\n",
      argv0);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      printf("%s\n", PERFETTO_GET_GIT_REVISION());
      return 0;
    }
  }

  if (argc < 2)
    return Usage(argv[0]);

  std::istream* input_stream;
  std::ifstream file_istream;
  if (argc > 2) {
    const char* file_path = argv[2];
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
  if (argc > 3) {
    const char* file_path = argv[3];
    file_ostream.open(file_path, std::ios_base::out | std::ios_base::trunc);
    if (!file_ostream.is_open())
      PERFETTO_FATAL("Could not open %s", file_path);
    output_stream = &file_ostream;
  } else {
    output_stream = &std::cout;
  }

  std::string format(argv[1]);

  if (format == "old_json")
    return perfetto::trace_to_text::TraceToSystrace(input_stream, output_stream,
                                                    /*wrap_in_json=*/true);
  if (format == "json")
    return perfetto::trace_to_text::TraceToExperimentalSystrace(
        input_stream, output_stream, /*wrap_in_json=*/true);
  if (format == "old_systrace")
    return perfetto::trace_to_text::TraceToSystrace(input_stream, output_stream,
                                                    /*wrap_in_json=*/false);
  if (format == "systrace")
    return perfetto::trace_to_text::TraceToExperimentalSystrace(
        input_stream, output_stream, /*wrap_in_json=*/false);
  if (format == "text")
    return perfetto::trace_to_text::TraceToText(input_stream, output_stream);

  if (format == "profile")
    return perfetto::trace_to_text::TraceToProfile(input_stream, output_stream);

  return Usage(argv[0]);
}
