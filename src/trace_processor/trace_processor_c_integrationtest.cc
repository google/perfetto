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

#include "perfetto/trace_processor/trace_processor_c.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

#include "perfetto/ext/base/scoped_mmap.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

constexpr char kSystrace[] =
    "# tracer: nop\n"
    "  sh-1 (1) [000] .... 10.000000: sched_switch: prev_comm=sh prev_pid=1 "
    "prev_prio=120 prev_state=S ==> next_comm=foo next_pid=2 next_prio=120\n"
    "  foo-2 (2) [000] .... 11.000000: sched_switch: prev_comm=foo prev_pid=2 "
    "prev_prio=120 prev_state=S ==> next_comm=sh next_pid=1 next_prio=120\n";

std::string ConsumeError(PerfettoTpError* err) {
  if (!err)
    return "";
  std::string msg = PerfettoTpErrorMessage(err);
  PerfettoTpErrorDestroy(err);
  return msg;
}

int64_t QueryInt(PerfettoTp* tp, const char* sql) {
  PerfettoTpIterator* it = PerfettoTpExecuteQuery(tp, sql);
  int64_t res = -1;
  if (PerfettoTpIteratorNext(it) &&
      PerfettoTpIteratorGetType(it, 0) == PERFETTO_TP_VALUE_TYPE_INT64) {
    res = PerfettoTpIteratorGetInt64(it, 0);
  }
  PerfettoTpIteratorDestroy(it);
  return res;
}

std::string GetBytes(PerfettoTpIterator* it, uint32_t col) {
  size_t size = 0;
  const uint8_t* data = PerfettoTpIteratorGetBytes(it, col, &size);
  return std::string(reinterpret_cast<const char*>(data), size);
}

TEST(TraceProcessorCApiTest, ParseAndQuery) {
  PerfettoTp* tp = PerfettoTpCreate();
  ASSERT_EQ(ConsumeError(
                PerfettoTpParse(tp, reinterpret_cast<const uint8_t*>(kSystrace),
                                sizeof(kSystrace) - 1, nullptr, nullptr)),
            "");
  ASSERT_EQ(ConsumeError(PerfettoTpNotifyEndOfFile(tp)), "");

  PerfettoTpIterator* it = PerfettoTpExecuteQuery(
      tp,
      "select name, 42 as i, 1.5 as d, null as n, x'0102' as b from thread "
      "where name = 'foo'");
  ASSERT_EQ(PerfettoTpIteratorColumnCount(it), 5u);
  EXPECT_STREQ(PerfettoTpIteratorColumnName(it, 0), "name");
  EXPECT_STREQ(PerfettoTpIteratorColumnName(it, 4), "b");

  ASSERT_TRUE(PerfettoTpIteratorNext(it));
  ASSERT_EQ(PerfettoTpIteratorGetType(it, 0), PERFETTO_TP_VALUE_TYPE_STRING);
  EXPECT_STREQ(PerfettoTpIteratorGetString(it, 0), "foo");
  ASSERT_EQ(PerfettoTpIteratorGetType(it, 1), PERFETTO_TP_VALUE_TYPE_INT64);
  EXPECT_EQ(PerfettoTpIteratorGetInt64(it, 1), 42);
  ASSERT_EQ(PerfettoTpIteratorGetType(it, 2), PERFETTO_TP_VALUE_TYPE_DOUBLE);
  EXPECT_EQ(PerfettoTpIteratorGetDouble(it, 2), 1.5);
  EXPECT_EQ(PerfettoTpIteratorGetType(it, 3), PERFETTO_TP_VALUE_TYPE_NULL);
  ASSERT_EQ(PerfettoTpIteratorGetType(it, 4), PERFETTO_TP_VALUE_TYPE_BYTES);
  EXPECT_EQ(GetBytes(it, 4), std::string("\x01\x02"));

  EXPECT_FALSE(PerfettoTpIteratorNext(it));
  EXPECT_EQ(ConsumeError(PerfettoTpIteratorStatus(it)), "");
  PerfettoTpIteratorDestroy(it);

  EXPECT_EQ(QueryInt(tp, "select count(*) from sched"), 2);
  PerfettoTpDestroy(tp);
}

TEST(TraceProcessorCApiTest, DeleterCalledOncePerChunk) {
  struct Chunk {
    std::string data;
    int* deleted;
  };
  int deleted = 0;
  auto deleter = [](void* ctx) {
    Chunk* chunk = static_cast<Chunk*>(ctx);
    ++*chunk->deleted;
    delete chunk;
  };

  PerfettoTp* tp = PerfettoTpCreate();
  std::string trace = kSystrace;
  size_t split = trace.size() / 2;
  for (std::string part : {trace.substr(0, split), trace.substr(split)}) {
    Chunk* chunk = new Chunk{std::move(part), &deleted};
    ASSERT_EQ(ConsumeError(PerfettoTpParse(
                  tp, reinterpret_cast<const uint8_t*>(chunk->data.data()),
                  chunk->data.size(), chunk, deleter)),
              "");
  }
  ASSERT_EQ(ConsumeError(PerfettoTpNotifyEndOfFile(tp)), "");
  EXPECT_EQ(QueryInt(tp, "select count(*) from thread where name = 'foo'"), 1);

  PerfettoTpDestroy(tp);
  EXPECT_EQ(deleted, 2);
}

TEST(TraceProcessorCApiTest, ParseMmapInChunks) {
  using SharedMmap = std::shared_ptr<base::ScopedMmap>;
  auto mmap = std::make_shared<base::ScopedMmap>(base::ReadMmapWholeFile(
      base::GetTestDataPath("test/data/example_android_trace_30s.pb")));
  if (!mmap->IsValid())
    GTEST_SKIP() << "mmap not supported";
  const uint8_t* data = static_cast<const uint8_t*>(mmap->data());
  size_t size = mmap->length();
  std::weak_ptr<base::ScopedMmap> weak = mmap;

  auto parse_in_chunks = [&](size_t chunk_size) {
    PerfettoTp* tp = PerfettoTpCreate();
    for (size_t off = 0; off < size; off += chunk_size) {
      EXPECT_EQ(ConsumeError(PerfettoTpParse(
                    tp, data + off, std::min(chunk_size, size - off),
                    new SharedMmap(mmap),
                    [](void* ctx) { delete static_cast<SharedMmap*>(ctx); })),
                "");
    }
    EXPECT_EQ(ConsumeError(PerfettoTpNotifyEndOfFile(tp)), "");
    int64_t count = QueryInt(tp, "select count(*) from sched");
    PerfettoTpDestroy(tp);
    return count;
  };

  int64_t whole = parse_in_chunks(size);
  EXPECT_GT(whole, 0);
  EXPECT_EQ(parse_in_chunks(64 * 1024), whole);

  mmap.reset();
  EXPECT_TRUE(weak.expired());
}

TEST(TraceProcessorCApiTest, QueryError) {
  PerfettoTp* tp = PerfettoTpCreate();
  ASSERT_EQ(ConsumeError(PerfettoTpNotifyEndOfFile(tp)), "");
  PerfettoTpIterator* it = PerfettoTpExecuteQuery(tp, "select * from nope");
  EXPECT_FALSE(PerfettoTpIteratorNext(it));
  EXPECT_NE(ConsumeError(PerfettoTpIteratorStatus(it)), "");
  PerfettoTpIteratorDestroy(it);
  PerfettoTpDestroy(tp);
}

}  // namespace
}  // namespace perfetto::trace_processor
