/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_redaction/collect_clocks.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/base/test/status_matchers.h"
#include "test/gtest_and_gmock.h"

#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/clock_snapshot.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/trace_packet_defaults.gen.h"

namespace perfetto::trace_redaction {

class CollectClocksTest : public testing::Test {
 protected:
  base::Status Collect() {
    std::string trace_buffer;
    for (auto&& packet : packets_) {
      protos::pbzero::TracePacket::Decoder decoder(packet.SerializeAsString());
      RETURN_IF_ERROR(collect_.Begin(&context_));
      RETURN_IF_ERROR(collect_.Collect(decoder, &context_));
      RETURN_IF_ERROR(collect_.End(&context_));
    }

    return base::OkStatus();
  }

  std::vector<protos::gen::TracePacket> packets_;
  Context context_;
  CollectClocks collect_;
};

TEST_F(CollectClocksTest, CollectsClocksAndConvertsPerfToTraceTs) {
  protos::gen::TracePacket trace_defaults_packet;
  auto* packet_defaults = trace_defaults_packet.mutable_trace_packet_defaults();

  // This is the perf samples clock
  packet_defaults->set_timestamp_clock_id(1);
  packets_.push_back(trace_defaults_packet);

  protos::gen::TracePacket clock_snapshot_packet;

  auto* clock_snapshot = clock_snapshot_packet.mutable_clock_snapshot();
  clock_snapshot->set_primary_trace_clock(
      static_cast<protos::gen::BuiltinClock>(4));
  auto* clocks = clock_snapshot->mutable_clocks();

  // Add a few clocks
  protos::gen::ClockSnapshot_Clock clock1;
  clock1.set_clock_id(4);
  clock1.set_timestamp(100);
  clocks->push_back(clock1);

  protos::gen::ClockSnapshot_Clock clock2;
  clock2.set_clock_id(1);
  clock2.set_timestamp(500);
  clocks->push_back(clock2);

  packets_.push_back(clock_snapshot_packet);

  ASSERT_OK(Collect());
  ASSERT_EQ(context_.clock_converter.GetPrimaryTraceClock(), 4);
  ASSERT_EQ(context_.clock_converter.GetPerfTraceClock(), 1);

  uint64_t trace_ts;
  context_.clock_converter.ConvertPerfToTrace(700, &trace_ts);
  ASSERT_EQ(trace_ts, 300u);  // 700 - 500 + 100 = 300

  context_.clock_converter.ConvertPerfToTrace(1000, &trace_ts);
  ASSERT_EQ(trace_ts, 600u);  // 1000 - 500 + 100
}

}  // namespace perfetto::trace_redaction
