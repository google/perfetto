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

#include "src/traceconv/trace_to_systrace.h"

#include <memory>
#include <sstream>
#include <string>

#include "perfetto/trace_processor/trace_processor.h"
#include "src/trace_processor/util/json_value.h"
#include "src/traceconv/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_to_text {
namespace {

TEST(ExtractSystrace, EscapesNamesInAndroidProcessDump) {
  const char kInputTrace[] = R"json({
    "traceEvents": [
      {
        "name": "process_name",
        "ph": "M",
        "pid": 10,
        "tid": 11,
        "args": {
          "name": "/dir-with\"quotes/out/Release/node\\\b\f\n\r\t\u0001\u3042"
        }
      },
      {
        "name": "thread_name",
        "ph": "M",
        "pid": 10,
        "tid": 11,
        "args": {"name": "thread\"\\\b\f\n\r\t\u0002"}
      }
    ]
  })json";

  std::istringstream input(kInputTrace);
  auto tp = trace_processor::TraceProcessor::CreateInstance(
      trace_processor::Config{});
  ASSERT_TRUE(ReadTraceUnfinalized(tp.get(), &input));
  base::Status status = tp->NotifyEndOfFile();
  ASSERT_TRUE(status.ok()) << status.message();

  std::ostringstream output;
  TraceWriter writer(&output);
  output << "{";
  ASSERT_EQ(
      ExtractSystrace(tp.get(), &writer, /*wrapped_in_json=*/true, Keep::kAll),
      0);
  output << "}";

  // Check source bytes because the in-tree parser accepts some invalid JSON,
  // including unescaped control characters in strings.
  const std::string output_json = output.str();
  EXPECT_EQ(output_json.find('\x01'), std::string::npos);
  EXPECT_EQ(output_json.find('\x02'), std::string::npos);
  EXPECT_THAT(
      output_json,
      testing::HasSubstr(
          "0000000000 S "
          R"json(/dir-with\"quotes/out/Release/node\\\b\f\n\r\t\u0001)json"
          "\xe3\x81\x82"));

  base::StatusOr<trace_processor::json::Dom> result =
      trace_processor::json::Parse(output_json);
  ASSERT_TRUE(result.ok()) << result.status().message() << "\n" << output_json;

  const std::string process_dump = (*result)["androidProcessDump"].AsString();
  EXPECT_THAT(process_dump, testing::HasSubstr(
                                "/dir-with\"quotes/out/Release/node\\\b\f\n\r\t"
                                "\x01\xe3\x81\x82"));
  EXPECT_THAT(process_dump, testing::HasSubstr("thread\"\\\b\f\n\r\t\x02"));
}

}  // namespace
}  // namespace perfetto::trace_to_text
