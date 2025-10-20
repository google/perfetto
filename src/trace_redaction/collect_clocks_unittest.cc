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
      std::string p = packet.SerializeAsString();
      protos::pbzero::TracePacket::Decoder decoder(p);
      RETURN_IF_ERROR(collect_.Begin(&context_));
      RETURN_IF_ERROR(collect_.Collect(decoder, &context_));
      RETURN_IF_ERROR(collect_.End(&context_));
    }

    return base::OkStatus();
  }

  void AddPerfTraceDefaultsToPacket(protos::gen::TracePacket& trace_packet,
                                    uint32_t trusted_seq_id,
                                    uint32_t clock_id) {
    auto* packet_defaults = trace_packet.mutable_trace_packet_defaults();

    trace_packet.set_trusted_packet_sequence_id(trusted_seq_id);

    packet_defaults->set_timestamp_clock_id(clock_id);

    // Create perf sample defaults so that the clock is detected as a perf
    // clock.
    packet_defaults->mutable_perf_sample_defaults();
  }

  std::vector<protos::gen::TracePacket> packets_;
  Context context_;
  CollectClocks collect_;
};

TEST_F(CollectClocksTest, CollectsClocksAndConvertsPerfToTraceTs) {
  // We need a trusted sequence id that will be used to map the clock ids.
  constexpr uint32_t trusted_sequence_id = 7;
  constexpr uint32_t perf_clock_id = 1;
  constexpr int trace_clock_id = 4;

  protos::gen::TracePacket trace_defaults_packet;
  AddPerfTraceDefaultsToPacket(trace_defaults_packet, trusted_sequence_id,
                               perf_clock_id);

  packets_.push_back(trace_defaults_packet);

  protos::gen::TracePacket clock_snapshot_packet;

  auto* clock_snapshot = clock_snapshot_packet.mutable_clock_snapshot();
  clock_snapshot->set_primary_trace_clock(
      static_cast<protos::gen::BuiltinClock>(trace_clock_id));
  auto* clocks = clock_snapshot->mutable_clocks();

  // Add a few clocks
  protos::gen::ClockSnapshot_Clock clock1;
  clock1.set_clock_id(trace_clock_id);
  clock1.set_timestamp(100);
  clocks->push_back(clock1);

  protos::gen::ClockSnapshot_Clock clock2;
  clock2.set_clock_id(perf_clock_id);
  clock2.set_timestamp(500);
  clocks->push_back(clock2);

  packets_.push_back(clock_snapshot_packet);

  ASSERT_OK(Collect());

  base::StatusOr<ClockId> primary_clock_id =
      context_.clock_converter.GetTraceClock();
  ASSERT_OK(primary_clock_id);
  ASSERT_EQ(primary_clock_id.value(), trace_clock_id);

  base::StatusOr<ClockId> clock_id =
      context_.clock_converter.GetDataSourceClock(
          trusted_sequence_id,
          RedactorClockConverter::DataSourceType::kPerfDataSource);
  ASSERT_OK(clock_id);
  ASSERT_EQ(clock_id.value(), perf_clock_id);
  base::StatusOr<uint64_t> trace_ts_1 =
      context_.clock_converter.ConvertToTrace(clock_id.value(), 700);
  ASSERT_OK(trace_ts_1);
  ASSERT_EQ(trace_ts_1.value(), 300u);  // 700 - 500 + 100 = 300

  clock_id = context_.clock_converter.GetDataSourceClock(
      trusted_sequence_id,
      RedactorClockConverter::DataSourceType::kPerfDataSource);
  ASSERT_OK(clock_id);
  ASSERT_EQ(clock_id.value(), perf_clock_id);
  base::StatusOr<uint64_t> trace_ts_2 =
      context_.clock_converter.ConvertToTrace(clock_id.value(), 1000);
  ASSERT_OK(trace_ts_2);
  ASSERT_EQ(trace_ts_2.value(), 600u);  // 1000 - 500 + 100
}

TEST_F(CollectClocksTest, CollectsClocksMultiSequence) {
  packets_.clear();
  constexpr int trace_clock_id = 4;

  // Create defaults for first trusted sequence
  constexpr int trusted_sequence_id_1 = 1;
  constexpr int perf_clock_id_seq_1 = 5;
  protos::gen::TracePacket trace_defaults_packet_seq_1;
  AddPerfTraceDefaultsToPacket(trace_defaults_packet_seq_1,
                               trusted_sequence_id_1, perf_clock_id_seq_1);
  packets_.push_back(trace_defaults_packet_seq_1);

  // Create defaults for second trusted sequence
  constexpr int trusted_sequence_id_2 = 2;
  constexpr int perf_clock_id_seq_2 = 6;
  protos::gen::TracePacket trace_defaults_packet_seq_2;
  AddPerfTraceDefaultsToPacket(trace_defaults_packet_seq_2,
                               trusted_sequence_id_2, perf_clock_id_seq_2);
  packets_.push_back(trace_defaults_packet_seq_2);

  protos::gen::TracePacket clock_snapshot_packet;

  auto* clock_snapshot = clock_snapshot_packet.mutable_clock_snapshot();
  clock_snapshot->set_primary_trace_clock(
      static_cast<protos::gen::BuiltinClock>(trace_clock_id));
  auto* clocks = clock_snapshot->mutable_clocks();

  // Add a few clocks
  protos::gen::ClockSnapshot_Clock clock1;
  clock1.set_clock_id(trace_clock_id);
  clock1.set_timestamp(100);
  clocks->push_back(clock1);

  protos::gen::ClockSnapshot_Clock clock2;
  clock2.set_clock_id(perf_clock_id_seq_1);
  clock2.set_timestamp(500);
  clocks->push_back(clock2);

  protos::gen::ClockSnapshot_Clock clock3;
  clock3.set_clock_id(perf_clock_id_seq_2);
  clock3.set_timestamp(800);
  clocks->push_back(clock3);
  packets_.push_back(clock_snapshot_packet);

  ASSERT_OK(Collect());
  base::StatusOr<ClockId> primary_clock_id =
      context_.clock_converter.GetTraceClock();
  ASSERT_OK(primary_clock_id);
  ASSERT_EQ(primary_clock_id.value(), trace_clock_id);

  base::StatusOr<ClockId> clock_id =
      context_.clock_converter.GetDataSourceClock(
          trusted_sequence_id_1,
          RedactorClockConverter::DataSourceType::kPerfDataSource);
  ASSERT_OK(clock_id);
  ASSERT_EQ(clock_id.value(), perf_clock_id_seq_1);
  base::StatusOr<uint64_t> trace_ts_1 =
      context_.clock_converter.ConvertToTrace(clock_id.value(), 700);
  ASSERT_OK(trace_ts_1);
  ASSERT_EQ(trace_ts_1.value(), 300u);  // 700 - 500 + 100 = 300

  clock_id = context_.clock_converter.GetDataSourceClock(
      trusted_sequence_id_2,
      RedactorClockConverter::DataSourceType::kPerfDataSource);
  ASSERT_OK(clock_id);
  ASSERT_EQ(clock_id.value(), perf_clock_id_seq_2);
  base::StatusOr<uint64_t> trace_ts_2 =
      context_.clock_converter.ConvertToTrace(clock_id.value(), 1000);
  ASSERT_OK(trace_ts_2);
  ASSERT_EQ(trace_ts_2.value(), 300u);  // 1000 - 800 + 100
}

}  // namespace perfetto::trace_redaction
