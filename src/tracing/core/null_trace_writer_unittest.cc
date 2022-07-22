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

#include "src/tracing/core/null_trace_writer.h"

#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "src/base/test/utils.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/trace/test_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace {

TEST(NullTraceWriterTest, WriterIdIsZero) {
  NullTraceWriter writer;
  EXPECT_EQ(writer.writer_id(), 0);
}

TEST(NullTraceWriterTest, NewTracePacket) {
  NullTraceWriter writer;
  for (size_t i = 0; i < 10000; i++) {
    auto packet = writer.NewTracePacket();
    packet->set_for_testing()->set_str("Hello, world!");
  }
}

#if defined(GTEST_HAS_DEATH_TEST)
TEST(NullTraceWriterDeathTest, NewTracePacketTakeStreamWriterNoFinish) {
  NullTraceWriter writer;
  auto trace_packet_handle = writer.NewTracePacket();
  protozero::ScatteredStreamWriter* ss = trace_packet_handle.TakeStreamWriter();
  const uint8_t payload[] = {'I', 'g', 'n', 'o', 'r', 'e', 'd'};
  ss->WriteBytes(payload, sizeof payload);

  EXPECT_DCHECK_DEATH(writer.NewTracePacket(););
}
#endif  // defined(GTEST_HAS_DEATH_TEST)

TEST(NullTraceWriterTest, NewTracePacketTakeStreamWriterWithFinish) {
  NullTraceWriter writer;
  for (size_t i = 0; i < 10000; i++) {
    auto trace_packet_handle = writer.NewTracePacket();
    protozero::ScatteredStreamWriter* ss =
        trace_packet_handle.TakeStreamWriter();
    const uint8_t payload[] = {'I', 'g', 'n', 'o', 'r', 'e', 'd'};
    ss->WriteBytes(payload, sizeof payload);
    writer.FinishTracePacket();
  }
}

TEST(NullTraceWriterTest, FlushCallbackIsCalled) {
  NullTraceWriter writer;
  writer.Flush();
  bool was_called = false;
  writer.Flush([&was_called] { was_called = true; });
  EXPECT_TRUE(was_called);
}

}  // namespace
}  // namespace perfetto
