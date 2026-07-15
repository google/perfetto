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

#include "src/trace_processor/shell/report/report_sink.h"
#include "src/trace_processor/shell/report/text_renderer.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace_processor/report.pbzero.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

using ::testing::HasSubstr;
using protos::pbzero::ReportPacket;

std::vector<uint8_t> HeaderPacket() {
  protozero::HeapBuffered<ReportPacket> p;
  auto* h = p->set_header();
  h->set_trace_file("t.pb");
  h->set_trace_dur_ns(12'300'000'000);
  h->set_process_count(12);
  return p.SerializeAsArray();
}

std::vector<uint8_t> SectionPacket() {
  using SI = protos::pbzero::SectionInfo;
  protozero::HeapBuffered<ReportPacket> p;
  auto* s = p->set_section_info();
  s->set_title("Slices");
  s->set_total_rows(100);
  s->set_shown_rows(2);
  s->set_total_items(1'000'000);
  s->set_item_noun("slice");
  s->set_row_noun("slice names");
  for (auto [name, fmt] : {std::pair<const char*, SI::ColumnFormat>
                               {"Name", SI::CF_STRING},
                           {"Count", SI::CF_COUNT},
                           {"Total dur", SI::CF_DURATION},
                           {"% of trace", SI::CF_PERCENT},
                           {"Max dur", SI::CF_DURATION}}) {
    auto* c = s->add_columns();
    c->set_name(name);
    c->set_format(fmt);
  }
  return p.SerializeAsArray();
}

std::vector<uint8_t> SlicePacket(const char* name,
                                 int64_t count,
                                 int64_t total_dur,
                                 int64_t max_dur,
                                 double pct) {
  protozero::HeapBuffered<ReportPacket> p;
  auto* s = p->set_slice_aggregate();
  s->set_name(name);
  s->set_count(count);
  s->set_total_dur_ns(total_dur);
  s->set_max_dur_ns(max_dur);
  s->set_pct_of_trace(pct);
  return p.SerializeAsArray();
}

// Reads a length-delimited varint. Returns false on truncation.
bool ReadVarint(const std::string& b, size_t* i, uint64_t* out) {
  uint64_t r = 0;
  int s = 0;
  while (*i < b.size()) {
    uint8_t x = static_cast<uint8_t>(b[(*i)++]);
    r |= static_cast<uint64_t>(x & 0x7f) << s;
    if (!(x & 0x80)) {
      *out = r;
      return true;
    }
    s += 7;
  }
  return false;
}

// Counts the packets in a length-delimited ReportPacket stream.
int CountPackets(const std::string& b) {
  size_t i = 0;
  int n = 0;
  while (i < b.size()) {
    uint64_t tag = 0;
    uint64_t len = 0;
    if (!ReadVarint(b, &i, &tag) || tag != 0x0A)
      return -1;
    if (!ReadVarint(b, &i, &len))
      return -1;
    if (i + len > b.size())
      return -1;
    i += len;
    n++;
  }
  return n;
}

TEST(ReportBinarySinkTest, FramesPacketsLengthDelimited) {
  base::TempFile tf = base::TempFile::Create();
  FILE* f = fopen(tf.path().c_str(), "wb");
  ASSERT_TRUE(f);
  BinarySink sink(f);
  auto h = HeaderPacket();
  auto s = SectionPacket();
  ASSERT_TRUE(sink.OnPacket({h.data(), h.size()}).ok());
  ASSERT_TRUE(sink.OnPacket({s.data(), s.size()}).ok());
  ASSERT_TRUE(sink.Finalize().ok());
  fclose(f);

  std::string out;
  ASSERT_TRUE(base::ReadFile(tf.path(), &out));
  EXPECT_EQ(CountPackets(out), 2);

  // A prefix cut inside the last packet still parses the first.
  EXPECT_EQ(CountPackets(out.substr(0, out.size() - 1)), -1)
      << "cut stream should not validate as a clean multiple of packets";
  size_t first_len = static_cast<uint8_t>(out[1]);
  EXPECT_EQ(CountPackets(out.substr(0, 2 + first_len)), 1);
}

TEST(ReportTextSinkTest, RendersHeaderTableAndFootnote) {
  base::TempFile tf = base::TempFile::Create();
  FILE* f = fopen(tf.path().c_str(), "wb");
  ASSERT_TRUE(f);
  TextSink sink(f, /*overview=*/false);

  auto emit = [&](const std::vector<uint8_t>& p) {
    ASSERT_TRUE(sink.OnPacket({p.data(), p.size()}).ok());
  };
  emit(HeaderPacket());
  emit(SectionPacket());
  emit(SlicePacket("Foo", 2000, 4'100'000'000, 128'000'000, 33.2));
  emit(SlicePacket("Bar", 500, 890'000'000, 42'000'000, 7.2));
  ASSERT_TRUE(sink.Finalize().ok());
  fclose(f);

  std::string out;
  ASSERT_TRUE(base::ReadFile(tf.path(), &out));

  EXPECT_THAT(out, HasSubstr("[t.pb | full trace | 12.3s | 12 processes]"));
  EXPECT_THAT(out, HasSubstr("Slices (1.0M total):"));
  // Human-formatted cells.
  EXPECT_THAT(out, HasSubstr("2.0k"));
  EXPECT_THAT(out, HasSubstr("4.1s"));
  EXPECT_THAT(out, HasSubstr("33.2%"));
  EXPECT_THAT(out, HasSubstr("128ms"));
  EXPECT_THAT(out, HasSubstr("890ms"));
  // Truncation footnote: 100 total, 2 shown.
  EXPECT_THAT(out, HasSubstr(
                       "98 more slice names below --top 2; rerun with --top 50"));
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
