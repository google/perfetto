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

#include <cstdint>
#include <memory>
#include <vector>

#include "src/trace_processor/importers/proto/network_trace_module.h"

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/perfetto/trace/android/network_trace.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::perfetto::protos::pbzero::TrafficDirection;

class NetworkTraceModuleTest : public testing::Test {
 public:
  NetworkTraceModuleTest() {
    context_.storage = std::make_shared<TraceStorage>();
    storage_ = context_.storage.get();

    context_.track_tracker = std::make_unique<TrackTracker>(&context_);
    context_.slice_tracker = std::make_unique<SliceTracker>(&context_);
    context_.args_tracker = std::make_unique<ArgsTracker>(&context_);
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(storage_);
    context_.slice_translation_table =
        std::make_unique<SliceTranslationTable>(storage_);
    context_.process_track_translation_table =
        std::make_unique<ProcessTrackTranslationTable>(storage_);
    context_.args_translation_table =
        std::make_unique<ArgsTranslationTable>(storage_);
    context_.async_track_set_tracker =
        std::make_unique<AsyncTrackSetTracker>(&context_);
    context_.proto_trace_parser =
        std::make_unique<ProtoTraceParserImpl>(&context_);
    context_.sorter = std::make_shared<TraceSorter>(
        &context_, TraceSorter::SortingMode::kFullSort);
  }

  base::Status TokenizeAndParse() {
    context_.chunk_readers.push_back(
        std::make_unique<ProtoTraceReader>(&context_));

    trace_->Finalize();
    std::vector<uint8_t> v = trace_.SerializeAsArray();
    trace_.Reset();

    auto status = context_.chunk_readers.back()->Parse(
        TraceBlobView(TraceBlob::CopyFrom(v.data(), v.size())));
    context_.sorter->ExtractEventsForced();
    context_.slice_tracker->FlushPendingSlices();
    context_.args_tracker->Flush();
    return status;
  }

  bool HasArg(ArgSetId sid, base::StringView key, Variadic value) {
    StringId key_id = storage_->InternString(key);
    const auto& a = storage_->arg_table();
    Query q;
    q.constraints = {a.arg_set_id().eq(sid)};
    for (auto it = a.FilterToIterator(q); it; ++it) {
      if (it.key() == key_id) {
        EXPECT_EQ(it.flat_key(), key_id);
        if (storage_->GetArgValue(it.row_number().row_number()) == value) {
          return true;
        }
      }
    }
    return false;
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
  EXPECT_EQ(slices[0].ts(), 123);

  EXPECT_TRUE(HasArg(2u, "packet_length", Variadic::Integer(72)));
  EXPECT_TRUE(HasArg(2u, "socket_uid", Variadic::Integer(1010)));
  EXPECT_TRUE(HasArg(2u, "local_port", Variadic::Integer(5100)));
  EXPECT_TRUE(HasArg(2u, "remote_port", Variadic::Integer(443)));
  EXPECT_TRUE(HasArg(2u, "packet_transport",
                     Variadic::String(storage_->InternString("IPPROTO_TCP"))));
  EXPECT_TRUE(HasArg(2u, "socket_tag",
                     Variadic::String(storage_->InternString("0x407"))));
  EXPECT_TRUE(HasArg(2u, "packet_tcp_flags",
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
  EXPECT_EQ(slices[0].ts(), 123);
  EXPECT_EQ(slices[1].ts(), 133);

  EXPECT_TRUE(HasArg(2u, "packet_length", Variadic::Integer(72)));
  EXPECT_TRUE(HasArg(3u, "packet_length", Variadic::Integer(100)));
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
  EXPECT_EQ(slices[0].ts(), 123);
  EXPECT_EQ(slices[0].dur(), 10);

  EXPECT_TRUE(HasArg(2u, "packet_length", Variadic::Integer(172)));
  EXPECT_TRUE(HasArg(2u, "packet_count", Variadic::Integer(2)));
}

}  // namespace
}  // namespace perfetto::trace_processor
