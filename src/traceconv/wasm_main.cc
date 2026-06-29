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

// Thin conversion-only entry point for the trace_processor "traceconv" WASM
// module used by the Perfetto UI (see ui/src/traceconv/index.ts). It exposes a
// trimmed traceconv-style CLI -- `<format> [input] [output]` -- covering only
// the format conversions the UI needs (json/systrace/profile and friends). The
// symbolize/deobfuscate/bundle modes are intentionally excluded: they are
// host-only and not invoked from the browser. The full host CLI lives in
// trace_processor_shell (the `convert`/`bundle`/`util` subcommands); this file
// is deliberately standalone so the WASM links only the lightweight
// src/traceconv:lib conversion code and not the rpc/httpd-heavy shell.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "src/protozero/text_to_proto/text_to_proto.h"
#include "src/traceconv/android_extension.descriptor.h"
#include "src/traceconv/trace.descriptor.h"
#include "src/traceconv/trace_to_firefox.h"
#include "src/traceconv/trace_to_json.h"
#include "src/traceconv/trace_to_profile.h"
#include "src/traceconv/trace_to_systrace.h"
#include "src/traceconv/trace_to_text.h"
#include "src/traceconv/trace_unpack.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace perfetto::traceconv {
namespace {

int Usage(const char* argv0) {
  fprintf(stderr,
          "Usage: %s MODE [OPTIONS] [input_file] [output_file]\n"
          "MODE is one of: json, systrace, ctrace, text, profile, "
          "java_heap_profile, firefox, decompress_packets, binary.\n",
          argv0);
  return 1;
}

uint64_t StringToUint64OrDie(const char* str) {
  std::optional<uint64_t> n = base::CStringToUInt64(str);
  if (!n) {
    PERFETTO_ELOG("Invalid %s. Expected decimal integer.", str);
    exit(1);
  }
  return *n;
}

int TextToTrace(std::istream* input, std::ostream* output) {
  std::string trace_text(std::istreambuf_iterator<char>{*input},
                         std::istreambuf_iterator<char>{});
  std::vector<uint8_t> descriptors;
  descriptors.reserve(kTraceDescriptor.size() +
                      kAndroidExtensionDescriptor.size());
  descriptors.insert(descriptors.end(), kTraceDescriptor.begin(),
                     kTraceDescriptor.end());
  descriptors.insert(descriptors.end(), kAndroidExtensionDescriptor.begin(),
                     kAndroidExtensionDescriptor.end());
  auto proto_status =
      protozero::TextToProto(descriptors.data(), descriptors.size(),
                             ".perfetto.protos.Trace", "trace", trace_text);
  if (!proto_status.ok()) {
    PERFETTO_ELOG("Failed to parse trace: %s",
                  proto_status.status().c_message());
    return 1;
  }
  const std::vector<uint8_t>& trace_proto = proto_status.value();
  output->write(reinterpret_cast<const char*>(trace_proto.data()),
                static_cast<int64_t>(trace_proto.size()));
  return 0;
}

int Main(int argc, char** argv) {
  std::vector<const char*> positional_args;
  trace_to_text::Keep truncate_keep = trace_to_text::Keep::kAll;
  uint64_t pid = 0;
  std::vector<uint64_t> timestamps;
  bool full_sort = false;
  std::optional<trace_to_text::ConversionMode> profile_type;
  bool profile_no_annotations = false;
  bool verbose = false;
  bool skip_unknown_fields = false;
  std::string output_dir;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--truncate") == 0) {
      i++;
      if (i < argc && strcmp(argv[i], "start") == 0) {
        truncate_keep = trace_to_text::Keep::kStart;
      } else if (i < argc && strcmp(argv[i], "end") == 0) {
        truncate_keep = trace_to_text::Keep::kEnd;
      } else {
        PERFETTO_ELOG("--truncate must specify whether to keep 'start' or 'end'.");
        return Usage(argv[0]);
      }
    } else if (i < argc && strcmp(argv[i], "--pid") == 0) {
      pid = StringToUint64OrDie(argv[++i]);
    } else if (i < argc && strcmp(argv[i], "--timestamps") == 0) {
      for (const std::string& ts : base::SplitString(argv[++i], ",")) {
        timestamps.emplace_back(StringToUint64OrDie(ts.c_str()));
      }
    } else if (strcmp(argv[i], "--alloc") == 0) {
      profile_type = trace_to_text::ConversionMode::kHeapProfile;
    } else if (strcmp(argv[i], "--perf") == 0) {
      profile_type = trace_to_text::ConversionMode::kPerfProfile;
    } else if (strcmp(argv[i], "--java-heap") == 0) {
      profile_type = trace_to_text::ConversionMode::kJavaHeapProfile;
    } else if (strcmp(argv[i], "--no-annotations") == 0) {
      profile_no_annotations = true;
    } else if (strcmp(argv[i], "--full-sort") == 0) {
      full_sort = true;
    } else if (strcmp(argv[i], "--verbose") == 0) {
      verbose = true;
    } else if (i < argc && strcmp(argv[i], "--output-dir") == 0) {
      output_dir = argv[++i];
    } else if (strcmp(argv[i], "--skip-unknown") == 0) {
      skip_unknown_fields = true;
    } else {
      positional_args.push_back(argv[i]);
    }
  }

  if (positional_args.empty())
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
    input_stream = &std::cin;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  std::ostream* output_stream;
  std::ofstream file_ostream;
  if (positional_args.size() > 2) {
    const char* file_path = positional_args[2];
    file_ostream.open(file_path, std::ios_base::out | std::ios_base::trunc |
                                     std::ios_base::binary);
    if (!file_ostream.is_open())
      PERFETTO_FATAL("Could not open %s", file_path);
    output_stream = &file_ostream;
  } else {
    output_stream = &std::cout;
  }

  std::string format(positional_args[0]);

  if ((format != "profile" && format != "java_heap_profile") &&
      (pid != 0 || !timestamps.empty())) {
    PERFETTO_ELOG(
        "--pid and --timestamps are supported only for profile formats.");
    return 1;
  }
  if ((format != "profile" && format != "java_heap_profile") &&
      !output_dir.empty()) {
    PERFETTO_ELOG("--output-dir is supported only for profile formats.");
    return 1;
  }

  if (format == "binary")
    return TextToTrace(input_stream, output_stream);

  if (format == "json")
    return trace_to_text::TraceToJson(input_stream, output_stream,
                                      /*compress=*/false, truncate_keep,
                                      full_sort);

  if (format == "systrace")
    return trace_to_text::TraceToSystrace(input_stream, output_stream,
                                          /*ctrace=*/false, truncate_keep,
                                          full_sort);

  if (format == "ctrace")
    return trace_to_text::TraceToSystrace(input_stream, output_stream,
                                          /*ctrace=*/true, truncate_keep,
                                          full_sort);

  if (truncate_keep != trace_to_text::Keep::kAll) {
    PERFETTO_ELOG("--truncate is unsupported for the '%s' format.",
                  format.c_str());
    return 1;
  }
  if (full_sort) {
    PERFETTO_ELOG("--full-sort is unsupported for the '%s' format.",
                  format.c_str());
    return 1;
  }

  if (format == "text") {
    trace_to_text::TraceToTextOptions options;
    options.skip_unknown_fields = skip_unknown_fields;
    return trace_to_text::TraceToText(input_stream, output_stream, options) ? 0
                                                                            : 1;
  }

  if (format == "profile")
    return trace_to_text::TraceToProfile(input_stream, pid, timestamps,
                                         !profile_no_annotations, output_dir,
                                         profile_type, verbose);

  if (format == "java_heap_profile")
    return trace_to_text::TraceToProfile(
        input_stream, pid, timestamps, !profile_no_annotations, output_dir,
        trace_to_text::ConversionMode::kJavaHeapProfile, verbose);

  if (format == "firefox")
    return trace_to_text::TraceToFirefoxProfile(input_stream, output_stream)
               ? 0
               : 1;

  if (format == "decompress_packets")
    return trace_to_text::UnpackCompressedPackets(input_stream, output_stream)
               ? 0
               : 1;

  return Usage(argv[0]);
}

}  // namespace
}  // namespace perfetto::traceconv

int main(int argc, char** argv) {
  return perfetto::traceconv::Main(argc, argv);
}
