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

#include "src/proto_utils/pb_to_txt.h"

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/perfetto_sql/structured_query.pbzero.h"
#include "protos/perfetto/trace_summary/file.gen.h"
#include "protos/perfetto/trace_summary/file.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using protos::gen::TraceConfig;
using protos::gen::TraceSummarySpec;

TEST(PbToTxtTest, EmptyTraceConfig) {
  TraceConfig tc;
  std::vector<uint8_t> data = tc.SerializeAsArray();
  std::string txt = TraceConfigPbToTxt(data.data(), data.size());
  EXPECT_EQ(txt, "");
}

TEST(PbToTxtTest, ValidTraceConfig) {
  TraceConfig tc;
  tc.set_duration_ms(1234);
  tc.set_trace_uuid_lsb(INT64_MAX);
  tc.set_trace_uuid_msb(1234567890124LL);
  auto* buf = tc.add_buffers();
  buf->set_size_kb(4096);
  buf->set_fill_policy(TraceConfig::BufferConfig::RING_BUFFER);
  tc.set_write_into_file(true);

  std::vector<uint8_t> data = tc.SerializeAsArray();
  std::string txt = TraceConfigPbToTxt(data.data(), data.size());
  EXPECT_EQ(txt, R"(buffers {
  size_kb: 4096
  fill_policy: RING_BUFFER
}
duration_ms: 1234
write_into_file: true
trace_uuid_msb: 1234567890124
trace_uuid_lsb: 9223372036854775807)");
}

TEST(PbToTxtTest, EmptyTraceSummarySpec) {
  TraceSummarySpec spec;
  std::vector<uint8_t> data = spec.SerializeAsArray();
  std::string txt = TraceSummarySpecPbToTxt(data.data(), data.size());
  EXPECT_EQ(txt, "");
}

TEST(PbToTxtTest, TraceSummarySpecWithQuery) {
  // Build the proto using pbzero and HeapBuffered (like other tests do)
  protozero::HeapBuffered<protos::pbzero::TraceSummarySpec> spec;
  auto* query = spec->add_query();
  query->set_id("test_query");
  auto* sql = query->set_sql();
  sql->set_sql("SELECT * FROM slice");

  std::vector<uint8_t> data = spec.SerializeAsArray();
  std::string txt = TraceSummarySpecPbToTxt(data.data(), data.size());
  EXPECT_EQ(txt, R"(query {
  id: "test_query"
  sql {
    sql: "SELECT * FROM slice"
  }
})");
}

}  // namespace
}  // namespace perfetto
