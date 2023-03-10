/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <fcntl.h>
#include <cstdio>
#include <memory>
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/read_trace.h"
#include "perfetto/trace_processor/trace_processor.h"

namespace perfetto {
namespace protoprofile {
namespace {

constexpr char kQuery[] = R"(
SELECT IMPORT('experimental.proto_path');

SELECT
  EXPERIMENTAL_PROFILE(
    EXPERIMENTAL_PROTO_PATH_TO_STACK(path_id),
    'size', 'bytes', size,
    'proto', 'count', count)
FROM EXPERIMENTAL_PROTO_CONTENT;

)";

int PrintUsage(int, const char** argv) {
  fprintf(stderr, "Usage: %s INPUT_PATH OUTPUT_PATH\n", argv[0]);
  return 1;
}

int Main(int argc, const char** argv) {
  if (argc != 3)
    return PrintUsage(argc, argv);

  const char* input_path = argv[1];
  const char* output_path = argv[2];

  trace_processor::Config config;
  config.analyze_trace_proto_content = true;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);
  base::Status read_status =
      trace_processor::ReadTrace(tp.get(), input_path, [](size_t parsed_size) {
        double size_mb = static_cast<double>(parsed_size) / 1E6;
        PERFETTO_DLOG("\rLoading trace: %.2f MB\r", size_mb);
      });
  if (!read_status.ok()) {
    PERFETTO_ELOG("Could not open input path (%s)", input_path);
    return 1;
  }

  auto it = tp->ExecuteQuery(kQuery);

  PERFETTO_CHECK(it.Next());
  PERFETTO_CHECK(it.ColumnCount() == 1);
  trace_processor::SqlValue value = it.Get(0);

  base::ScopedFile output_fd =
      base::OpenFile(output_path, O_WRONLY | O_TRUNC | O_CREAT, 0600);
  if (!output_fd) {
    PERFETTO_ELOG("Could not open output path (%s)", output_path);
    return 1;
  }
  base::WriteAll(output_fd.get(), value.AsBytes(), value.bytes_count);
  base::FlushFile(output_fd.get());

  PERFETTO_CHECK(!it.Next());

  return 0;
}

}  // namespace
}  // namespace protoprofile
}  // namespace perfetto

int main(int argc, const char** argv) {
  return perfetto::protoprofile::Main(argc, argv);
}
