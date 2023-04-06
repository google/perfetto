/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/network_trace_module.h"

#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/proto_trace_parser.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {
using ::perfetto::protos::pbzero::TrafficDirection;

class NetworkTraceModuleTest : public testing::Test {
 public:
  NetworkTraceModuleTest() {
    context_.storage.reset(new TraceStorage());
    storage_ = context_.storage.get();

    context_.track_tracker.reset(new TrackTracker(&context_));
    context_.slice_tracker.reset(new SliceTracker(&context_));
    context_.args_tracker.reset(new ArgsTracker(&context_));
    context_.global_args_tracker.reset(new GlobalArgsTracker(storage_));
    context_.slice_translation_table.reset(new SliceTranslationTable(storage_));
    context_.args_translation_table.reset(new ArgsTranslationTable(storage_));
    context_.async_track_set_tracker.reset(new AsyncTrackSetTracker(&context_));
    context_.sorter.reset(new TraceSorter(
        &context_, std::make_unique<ProtoTraceParser>(&context_),
        TraceSorter::SortingMode::kFullSort));
  }

  util::Status TokenizeAndParse() {
    context_.chunk_reader.reset(new ProtoTraceReader(&context_));

    trace_->Finalize();
    std::vector<uint8_t> v = trace_.SerializeAsArray();
    trace_.Reset();

    auto status = context_.chunk_reader->Parse(
        TraceBlobView(TraceBlob::CopyFrom(v.data(), v.size())));
    context_.sorter->ExtractEventsForced();
    context_.slice_tracker->FlushPendingSlices();
    context_.args_tracker->Flush();
    return status;
  }

  bool HasArg(ArgSetId set_id, base::StringView key, Variadic value) {
    StringId key_id = storage_->InternString(key);
    const auto& args = storage_->arg_table();
    RowMap rm = args.FilterToRowMap({args.arg_set_id().eq(set_id)});
    bool found = false;
    for (auto it = rm.IterateRows(); it; it.Next()) {
      if (args.key()[it.index()] == key_id) {
        EXPECT_EQ(args.flat_key()[it.index()], key_id);
        if (storage_->GetArgValue(it.index()) == value) {
          found = true;
          break;
        }
      }
    }
    return found;
  }

 protected:
  protozero::HeapBuffered<protos::pbzero::Trace> trace_;
  TraceProcessorContext context_;
  TraceStorage* storage_;
};

TEST_F(NetworkTraceModuleTest, ParseAndFormatPacket) {
  NetworkTraceModule module(&context_);

  auto* packet = trace_->add_packet();
  packet->set_timestamp(123);

  auto* event = packet->set_network_packet();
  event->set_direction(TrafficDirection::DIR_EGRESS);
  event->set_length(72);
  event->set_uid(1010);
  event->set_tag(0x407);
  event->set_local_port(5100);
  event->set_remote_port(443);
  event->set_tcp_flags(0b10010);
  event->set_ip_proto(6);
  event->set_interface("wlan");

  ASSERT_TRUE(TokenizeAndParse().ok());

  const auto& slices = storage_->slice_table();
  ASSERT_EQ(slices.row_count(), 1u);
  EXPECT_EQ(slices.ts()[0], 123);

  EXPECT_TRUE(HasArg(1u, "packet_length", Variadic::Integer(72)));
  EXPECT_TRUE(HasArg(1u, "socket_uid", Variadic::Integer(1010)));
  EXPECT_TRUE(HasArg(1u, "local_port", Variadic::Integer(5100)));
  EXPECT_TRUE(HasArg(1u, "remote_port", Variadic::Integer(443)));
  EXPECT_TRUE(HasArg(1u, "packet_transport",
                     Variadic::String(storage_->InternString("IPPROTO_TCP"))));
  EXPECT_TRUE(HasArg(1u, "socket_tag",
                     Variadic::String(storage_->InternString("0x407"))));
  EXPECT_TRUE(HasArg(1u, "packet_tcp_flags",
                     Variadic::String(storage_->InternString(".s..a..."))));
}

TEST_F(NetworkTraceModuleTest, TokenizeAndParsePerPacketBundle) {
  NetworkTraceModule module(&context_);

  auto* packet = trace_->add_packet();
  packet->set_timestamp(123);

  protozero::PackedVarInt timestamps;
  timestamps.Append(0);
  timestamps.Append(10);

  protozero::PackedVarInt lengths;
  lengths.Append(72);
  lengths.Append(100);

  auto* event = packet->set_network_packet_bundle();
  event->set_packet_timestamps(timestamps);
  event->set_packet_lengths(lengths);

  auto* ctx = event->set_ctx();
  ctx->set_uid(456);

  ASSERT_TRUE(TokenizeAndParse().ok());

  const auto& slices = storage_->slice_table();
  ASSERT_EQ(slices.row_count(), 2u);
  EXPECT_EQ(slices.ts()[0], 123);
  EXPECT_EQ(slices.ts()[1], 133);

  EXPECT_TRUE(HasArg(1u, "packet_length", Variadic::Integer(72)));
  EXPECT_TRUE(HasArg(2u, "packet_length", Variadic::Integer(100)));
}

TEST_F(NetworkTraceModuleTest, TokenizeAndParseAggregateBundle) {
  NetworkTraceModule module(&context_);

  auto* packet = trace_->add_packet();
  packet->set_timestamp(123);

  auto* event = packet->set_network_packet_bundle();
  event->set_total_packets(2);
  event->set_total_duration(10);
  event->set_total_length(172);

  auto* ctx = event->set_ctx();
  ctx->set_uid(456);

  ASSERT_TRUE(TokenizeAndParse().ok());

  const auto& slices = storage_->slice_table();
  ASSERT_EQ(slices.row_count(), 1u);
  EXPECT_EQ(slices.ts()[0], 123);
  EXPECT_EQ(slices.dur()[0], 10);

  EXPECT_TRUE(HasArg(1u, "packet_length", Variadic::UnsignedInteger(172)));
  EXPECT_TRUE(HasArg(1u, "packet_count", Variadic::UnsignedInteger(2)));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
