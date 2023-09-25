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

#include "src/bigtrace/trace_processor_wrapper.h"
#include <cstdint>
#include <optional>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/platform_handle.h"
#include "perfetto/base/status.h"
#include "perfetto/base/time.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/threading/stream.h"
#include "perfetto/ext/base/threading/thread_pool.h"
#include "perfetto/ext/base/threading/util.h"
#include "protos/perfetto/bigtrace/worker.pb.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace bigtrace {
namespace {

using SF = TraceProcessorWrapper::Statefulness;

const char kSimpleSystrace[] = R"--(# tracer
  surfaceflinger-598   (  598) [004] .... 10852.771242: tracing_mark_write: B|598|some event
  surfaceflinger-598   (  598) [004] .... 10852.771245: tracing_mark_write: E|598
)--";

base::StatusOr<std::vector<uint8_t>> SimpleSystrace() {
  return std::vector<uint8_t>(kSimpleSystrace,
                              kSimpleSystrace + strlen(kSimpleSystrace));
}

std::vector<base::StatusOr<std::vector<uint8_t>>> SimpleSystraceChunked() {
  std::string systrace(kSimpleSystrace);
  std::vector<base::StatusOr<std::vector<uint8_t>>> chunks;
  for (auto& chunk : base::SplitString(systrace, "\n")) {
    auto with_newline = chunk + "\n";
    chunks.push_back(std::vector<uint8_t>(
        with_newline.data(), with_newline.data() + with_newline.size()));
  }

  return chunks;
}

template <typename T>
std::optional<T> WaitForFutureReady(base::Future<T>& future,
                                    std::optional<uint32_t> timeout_ms) {
  base::FlatSet<base::PlatformHandle> ready;
  base::FlatSet<base::PlatformHandle> interested;
  base::PollContext ctx(&interested, &ready);
  auto res = future.Poll(&ctx);
  for (; res.IsPending(); res = future.Poll(&ctx)) {
    PERFETTO_CHECK(interested.size() == 1);
    if (!base::BlockUntilReadableFd(*interested.begin(), timeout_ms)) {
      return std::nullopt;
    }
    interested = {};
  }
  return res.item();
}

template <typename T>
T WaitForFutureReady(base::Future<T>& future) {
  return *WaitForFutureReady(future, std::nullopt);
}

template <typename T>
std::optional<T> WaitForStreamReady(base::Stream<T>& stream) {
  base::FlatSet<base::PlatformHandle> ready;
  base::FlatSet<base::PlatformHandle> interested;
  base::PollContext ctx(&interested, &ready);
  auto res = stream.PollNext(&ctx);
  for (; res.IsPending(); res = stream.PollNext(&ctx)) {
    PERFETTO_CHECK(interested.size() == 1);
    base::BlockUntilReadableFd(*interested.begin());
    interested = {};
  }
  return res.IsDone() ? std::nullopt : std::make_optional(res.item());
}

TEST(TraceProcessorWrapperUnittest, Stateful) {
  base::ThreadPool pool(1);
  TraceProcessorWrapper wrapper("foobar", &pool, SF::kStateful);
  {
    auto load = wrapper.LoadTrace(base::StreamOf(SimpleSystrace()));
    base::Status status = WaitForFutureReady(load);
    ASSERT_TRUE(status.ok()) << status.message();
  }
  {
    auto stream = wrapper.Query("CREATE VIEW foo AS SELECT ts, dur FROM slice");
    auto proto = WaitForStreamReady(stream);
    ASSERT_TRUE(proto.has_value());
    ASSERT_TRUE(proto->ok()) << proto->status().message();

    ASSERT_FALSE(WaitForStreamReady(stream).has_value());
  }
  {
    auto stream = wrapper.Query("SELECT ts, dur FROM foo");
    auto proto = WaitForStreamReady(stream);

    ASSERT_TRUE(proto.has_value());
    ASSERT_TRUE(proto->ok()) << proto->status().message();

    ASSERT_EQ(proto->value().trace(), "foobar");

    auto& result = proto.value()->result();
    ASSERT_EQ(result.batch_size(), 1);
    ASSERT_EQ(result.batch(0).cells_size(), 2);

    ASSERT_EQ(result.batch(0).cells(0),
              protos::QueryResult::CellsBatch::CELL_VARINT);
    ASSERT_EQ(result.batch(0).cells(1),
              protos::QueryResult::CellsBatch::CELL_VARINT);
    ASSERT_EQ(result.batch(0).varint_cells(0), 10852771242000);
    ASSERT_EQ(result.batch(0).varint_cells(1), 3000);

    ASSERT_FALSE(WaitForStreamReady(stream).has_value());
  }
}

TEST(TraceProcessorWrapperUnittest, Stateless) {
  base::ThreadPool pool(1);
  TraceProcessorWrapper wrapper("foobar", &pool, SF::kStateless);
  {
    auto load = wrapper.LoadTrace(base::StreamOf(SimpleSystrace()));
    base::Status status = WaitForFutureReady(load);
    ASSERT_TRUE(status.ok()) << status.message();
  }
  {
    auto stream = wrapper.Query("CREATE VIEW foo AS SELECT ts, dur FROM slice");
    auto proto = WaitForStreamReady(stream);
    ASSERT_TRUE(proto.has_value());
    ASSERT_TRUE(proto->ok()) << proto->status().message();

    ASSERT_FALSE(WaitForStreamReady(stream).has_value());
  }

  // Second CREATE VIEW should also succeed because the first one should have
  // been wiped.
  {
    auto stream = wrapper.Query("CREATE VIEW foo AS SELECT ts, dur FROM slice");
    auto proto = WaitForStreamReady(stream);
    ASSERT_TRUE(proto.has_value());
    ASSERT_TRUE(proto->ok()) << proto->status().message();

    ASSERT_FALSE(WaitForStreamReady(stream).has_value());
  }

  // Selecting from it should return an error.
  {
    auto stream = wrapper.Query("SELECT ts, dur FROM foo");
    auto proto = WaitForStreamReady(stream);
    ASSERT_TRUE(proto.has_value());
    ASSERT_TRUE(proto->ok()) << proto->status().message();
    ASSERT_TRUE(proto->value().result().has_error());

    ASSERT_FALSE(WaitForStreamReady(stream).has_value());
  }
}

TEST(TraceProcessorWrapperUnittest, Chunked) {
  base::ThreadPool pool(1);
  TraceProcessorWrapper wrapper("foobar", &pool, SF::kStateless);
  {
    auto chunked = SimpleSystraceChunked();
    ASSERT_EQ(chunked.size(), 3u);
    auto load = wrapper.LoadTrace(base::StreamFrom(chunked));
    base::Status status = WaitForFutureReady(load);
    ASSERT_TRUE(status.ok()) << status.message();
  }
  {
    auto stream = wrapper.Query("SELECT ts, dur FROM slice");
    auto proto = WaitForStreamReady(stream);

    ASSERT_TRUE(proto.has_value());
    ASSERT_TRUE(proto->ok()) << proto->status().message();

    ASSERT_EQ(proto->value().trace(), "foobar");

    auto& result = proto.value()->result();
    ASSERT_EQ(result.batch_size(), 1);
    ASSERT_EQ(result.batch(0).cells_size(), 2);

    ASSERT_EQ(result.batch(0).cells(0),
              protos::QueryResult::CellsBatch::CELL_VARINT);
    ASSERT_EQ(result.batch(0).cells(1),
              protos::QueryResult::CellsBatch::CELL_VARINT);
    ASSERT_EQ(result.batch(0).varint_cells(0), 10852771242000);
    ASSERT_EQ(result.batch(0).varint_cells(1), 3000);

    ASSERT_FALSE(WaitForStreamReady(stream).has_value());
  }
}

TEST(TraceProcessorWrapperUnittest, Interrupt) {
  base::ThreadPool pool(1);
  TraceProcessorWrapper wrapper("foobar", &pool, SF::kStateful);

  // Create a query which will run ~forever. When this stream is dropped we
  // should propogate to the TP instance to also stop running the query.
  {
    auto stream = wrapper.Query(
        "WITH RECURSIVE nums AS ( "
        "SELECT 1 num "
        "UNION "
        "SELECT num + 1 from nums WHERE num < 100000000000000) "
        "SELECT COUNT(num) FROM nums");

    // Wait for a bit for the thread to start running. To do something better
    // we would need a way to figure out that the thread has started executing
    // so we could stop. Unfortunately, this is quite a difficult problem to
    // solve and probably not worth doing.
    base::SleepMicroseconds(10 * 1000);
  }

  // Verify that we are able to run something on the thread pool in a reasonable
  // amount of time.
  {
    auto future = base::RunOnceOnThreadPool<int>(&pool, []() { return 1; });
    ASSERT_EQ(WaitForFutureReady(future, 250), 1);
  }
}

}  // namespace
}  // namespace bigtrace
}  // namespace perfetto
