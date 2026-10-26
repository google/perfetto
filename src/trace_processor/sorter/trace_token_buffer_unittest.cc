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

#include "src/trace_processor/sorter/trace_token_buffer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#include "perfetto/base/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/bump_allocator.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

// A blob whose bytes differ at every offset, so a copy which lands on the
// wrong bytes is caught.
TraceBlob PatternBlob(size_t size) {
  TraceBlob blob = TraceBlob::Allocate(size);
  for (size_t i = 0; i < size; ++i) {
    blob.mutable_data()[i] = static_cast<uint8_t>(i * 7 + i / 251);
  }
  return blob;
}

// Packets are copied into the buffer, so they come back with the same bytes
// but not as the same view.
bool SameBytes(const TraceBlobView& a, const TraceBlobView& b) {
  return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

class TraceTokenBufferUnittest : public testing::Test {
 public:
  TraceTokenBufferUnittest() {
    context.storage.reset(new TraceStorage());
    context.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context.storage.get());
    context.machine_tracker.reset(
        new MachineTracker(&context, kDefaultMachineId));
    context.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId{0}});
    context.stats_tracker = std::make_unique<StatsTracker>(&context);
    state = PacketSequenceStateGeneration::CreateFirst(
        &context, /*trusted_packet_sequence_id=*/1);
  }

 protected:
  TraceTokenBuffer store{/*keep_track_event_packets=*/false};
  TraceProcessorContext context;
  RefPtr<PacketSequenceStateGeneration> state;
};

TEST_F(TraceTokenBufferUnittest, TracePacketDataInOut) {
  TraceBlobView tbv(PatternBlob(1024));
  TracePacketData tpd{tbv.copy(), state.get()};

  TraceTokenBuffer::Id id = store.Append(std::move(tpd));
  TracePacketData extracted = store.Extract<TracePacketData>(id);
  ASSERT_TRUE(SameBytes(extracted.packet, tbv));
  ASSERT_EQ(extracted.sequence_state, state.get());
}

TEST_F(TraceTokenBufferUnittest, PacketAppendMultipleBlobs) {
  TraceBlobView tbv_1(PatternBlob(1024));
  TraceBlobView tbv_2(PatternBlob(2048));
  TraceBlobView tbv_3(PatternBlob(4096));

  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{tbv_1.copy(), state.get()});
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{tbv_2.copy(), state.get()});
  ASSERT_TRUE(SameBytes(store.Extract<TracePacketData>(id_1).packet, tbv_1));
  ASSERT_TRUE(SameBytes(store.Extract<TracePacketData>(id_2).packet, tbv_2));

  TraceTokenBuffer::Id id_3 =
      store.Append(TracePacketData{tbv_3.copy(), state.get()});
  ASSERT_TRUE(SameBytes(store.Extract<TracePacketData>(id_3).packet, tbv_3));
}

TEST_F(TraceTokenBufferUnittest, BlobSharing) {
  TraceBlobView root(PatternBlob(2048));
  TraceBlobView tbv_1 = root.slice_off(0, 1024);
  TraceBlobView tbv_2 = root.slice_off(1024, 512);
  TraceBlobView tbv_3 = root.slice_off(1536, 512);

  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{tbv_1.copy(), state.get()});
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{tbv_2.copy(), state.get()});
  ASSERT_TRUE(SameBytes(store.Extract<TracePacketData>(id_1).packet, tbv_1));
  ASSERT_TRUE(SameBytes(store.Extract<TracePacketData>(id_2).packet, tbv_2));

  TraceTokenBuffer::Id id_3 =
      store.Append(TracePacketData{tbv_3.copy(), state.get()});
  ASSERT_TRUE(SameBytes(store.Extract<TracePacketData>(id_3).packet, tbv_3));
}

TEST_F(TraceTokenBufferUnittest, SequenceStateSharing) {
  TraceBlobView root(PatternBlob(2048));
  TraceBlobView tbv_1 = root.slice_off(0, 1024);
  TraceBlobView tbv_2 = root.slice_off(1024, 512);

  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{tbv_1.copy(), state.get()});
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{tbv_2.copy(), state.get()});
  ASSERT_EQ(store.Extract<TracePacketData>(id_1).sequence_state, state.get());
  ASSERT_EQ(store.Extract<TracePacketData>(id_2).sequence_state, state.get());
}

TEST_F(TraceTokenBufferUnittest, ManySequenceState) {
  TraceBlobView root(PatternBlob(1024));

  std::array<TraceTokenBuffer::Id, 1024> ids;
  std::array<PacketSequenceStateGeneration*, 1024> refs;
  for (uint32_t i = 0; i < 1024; ++i) {
    refs[i] = state.get();
    ids[i] = store.Append(TracePacketData{root.slice_off(i, 1), state.get()});
    state = state->OnNewTracePacketDefaults(TraceBlobView());
  }

  for (uint32_t i = 0; i < 1024; ++i) {
    ASSERT_EQ(refs[i], store.Extract<TracePacketData>(ids[i]).sequence_state);
  }
}

TEST_F(TraceTokenBufferUnittest, PacketLargeOffset) {
  TraceBlobView tbv(PatternBlob(256ul * 1024));

  TraceBlobView slice_1 = tbv.slice_off(0, 1024ul);
  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{slice_1.copy(), state.get()});
  TracePacketData out_1 = store.Extract<TracePacketData>(id_1);
  ASSERT_TRUE(SameBytes(out_1.packet, slice_1));
  ASSERT_EQ(out_1.sequence_state, state.get());

  TraceBlobView slice_2 = tbv.slice_off(128ul * 1024, 1024ul);
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{slice_2.copy(), state.get()});
  TracePacketData out_2 = store.Extract<TracePacketData>(id_2);
  ASSERT_TRUE(SameBytes(out_2.packet, slice_2));
  ASSERT_EQ(out_2.sequence_state, state.get());

  // A packet larger than a chunk gets a heap copy of its own.
  TraceBlobView slice_3 = tbv.slice_off(1024ul, BumpAllocator::kChunkSize + 1);
  TraceTokenBuffer::Id id_3 =
      store.Append(TracePacketData{slice_3.copy(), state.get()});
  TracePacketData out_3 = store.Extract<TracePacketData>(id_3);
  ASSERT_TRUE(SameBytes(out_3.packet, slice_3));
  ASSERT_EQ(out_3.sequence_state, state.get());
}

TEST_F(TraceTokenBufferUnittest, TrackEventDataInOut) {
  TraceBlobView tbv(PatternBlob(1234));
  TrackEventData ted(tbv.copy(), state.get(), std::nullopt);
  ted.thread_instruction_count = 123;
  ted.extra_counter_values = {10, 2, 0, 0, 0, 0, 0, 0};
  auto counter_array = ted.extra_counter_values;

  TraceTokenBuffer::Id id = store.Append(std::move(ted));
  TrackEventData extracted = store.Extract<TrackEventData>(id);
  ASSERT_TRUE(SameBytes(extracted.event, tbv));
  ASSERT_EQ(extracted.packet, std::nullopt);
  ASSERT_EQ(extracted.sequence_state, state.get());
  ASSERT_EQ(extracted.thread_instruction_count, 123);
  ASSERT_EQ(extracted.thread_timestamp, std::nullopt);
  ASSERT_DOUBLE_EQ(extracted.counter_value, 0.0);
  ASSERT_EQ(extracted.extra_counter_values, counter_array);
}

TEST_F(TraceTokenBufferUnittest, TrackEventDataWithPacketInOut) {
  TraceTokenBuffer keeping_store{/*keep_track_event_packets=*/true};
  TraceBlobView packet(PatternBlob(1234));
  TraceBlobView event = packet.slice_off(100, 200);
  TrackEventData ted(event.copy(), state.get(), packet.copy());
  ted.thread_timestamp = 456;

  TraceTokenBuffer::Id id = keeping_store.Append(std::move(ted));
  TrackEventData extracted = keeping_store.Extract<TrackEventData>(id);
  ASSERT_TRUE(SameBytes(extracted.event, event));
  ASSERT_TRUE(extracted.packet);
  ASSERT_TRUE(SameBytes(*extracted.packet, packet));
  ASSERT_EQ(extracted.sequence_state, state.get());
  ASSERT_EQ(extracted.thread_timestamp, 456);
}

TEST_F(TraceTokenBufferUnittest, FtraceDataInOutWithoutRawTs) {
  TraceBlobView tbv(PatternBlob(1024));
  FtraceData data{tbv.copy(), state.get(), FtraceData::kRawTsUnset};

  TraceTokenBuffer::Id id = store.Append(std::move(data));
  FtraceData extracted = store.Extract<FtraceData>(id);
  ASSERT_TRUE(SameBytes(extracted.packet, tbv));
  ASSERT_EQ(extracted.sequence_state, state.get());
  ASSERT_EQ(extracted.raw_ts, FtraceData::kRawTsUnset);
}

TEST_F(TraceTokenBufferUnittest, FtraceDataInOutWithRawTs) {
  TraceBlobView tbv(PatternBlob(1024));
  constexpr int64_t kRawTs = 1234567890123;
  FtraceData data{tbv.copy(), state.get(), kRawTs};

  TraceTokenBuffer::Id id = store.Append(std::move(data));
  FtraceData extracted = store.Extract<FtraceData>(id);
  ASSERT_TRUE(SameBytes(extracted.packet, tbv));
  ASSERT_EQ(extracted.sequence_state, state.get());
  ASSERT_EQ(extracted.raw_ts, kRawTs);
}

TEST_F(TraceTokenBufferUnittest, FtraceDataMixedRawTsPresence) {
  // Interleave entries with and without raw_ts to ensure the variable-length
  // encoding doesn't desync between adjacent allocations.
  TraceBlobView root(PatternBlob(2048));
  TraceBlobView tbv_1 = root.slice_off(0, 512);
  TraceBlobView tbv_2 = root.slice_off(512, 512);
  TraceBlobView tbv_3 = root.slice_off(1024, 512);
  constexpr int64_t kRawTs1 = 100;
  constexpr int64_t kRawTs3 = 300;

  TraceTokenBuffer::Id id_1 =
      store.Append(FtraceData{tbv_1.copy(), state.get(), kRawTs1});
  TraceTokenBuffer::Id id_2 = store.Append(
      FtraceData{tbv_2.copy(), state.get(), FtraceData::kRawTsUnset});
  TraceTokenBuffer::Id id_3 =
      store.Append(FtraceData{tbv_3.copy(), state.get(), kRawTs3});

  FtraceData out_1 = store.Extract<FtraceData>(id_1);
  ASSERT_TRUE(SameBytes(out_1.packet, tbv_1));
  ASSERT_EQ(out_1.raw_ts, kRawTs1);

  FtraceData out_2 = store.Extract<FtraceData>(id_2);
  ASSERT_TRUE(SameBytes(out_2.packet, tbv_2));
  ASSERT_EQ(out_2.raw_ts, FtraceData::kRawTsUnset);

  FtraceData out_3 = store.Extract<FtraceData>(id_3);
  ASSERT_TRUE(SameBytes(out_3.packet, tbv_3));
  ASSERT_EQ(out_3.raw_ts, kRawTs3);
}

TEST_F(TraceTokenBufferUnittest, ExtractOrAppendAfterFreeMemory) {
  auto unused_res = store.Extract<TraceBlobView>(
      store.Append(TraceBlobView(PatternBlob(1234))));
  base::ignore_result(unused_res);

  store.FreeMemory();

  TraceTokenBuffer::Id id = store.Append(TraceBlobView(PatternBlob(4567)));
  TraceBlobView tbv = store.Extract<TraceBlobView>(id);
  ASSERT_EQ(tbv.size(), 4567u);
}

TEST_F(TraceTokenBufferUnittest, GenericExtractDrainsChunk) {
  struct alignas(8) Payload {
    int64_t value;
  };
  RefPtr<PacketSequenceStateGeneration> other =
      PacketSequenceStateGeneration::CreateFirst(
          &context, /*trusted_packet_sequence_id=*/2);

  // A straggler which fills the first chunk keeps it, and every chunk after
  // it, from being erased.
  TraceBlobView straggler(PatternBlob(BumpAllocator::kChunkSize - 16));
  TraceTokenBuffer::Id straggler_id =
      store.Append(TracePacketData{straggler.copy(), state.get()});

  // The second chunk is drained by a generic extract.
  TraceBlobView tbv(PatternBlob(128));
  TraceTokenBuffer::Id packet_id =
      store.Append(TracePacketData{tbv.copy(), other.get()});
  TraceTokenBuffer::Id payload_id = store.Append(Payload{42});
  ASSERT_NE(packet_id.alloc_id.chunk_index, straggler_id.alloc_id.chunk_index);
  ASSERT_EQ(payload_id.alloc_id.chunk_index, packet_id.alloc_id.chunk_index);

  TracePacketData out = store.Extract<TracePacketData>(packet_id);
  ASSERT_TRUE(SameBytes(out.packet, tbv));
  ASSERT_EQ(store.Extract<Payload>(payload_id).value, 42);
  ASSERT_FALSE(other->HasOneRef());

  // The drained chunk drops its generation even though it cannot be erased.
  store.FreeMemory();
  EXPECT_TRUE(other->HasOneRef());

  TracePacketData straggler_out = store.Extract<TracePacketData>(straggler_id);
  ASSERT_TRUE(SameBytes(straggler_out.packet, straggler));
  ASSERT_EQ(straggler_out.sequence_state, state.get());
  store.FreeMemory();
  ASSERT_TRUE(state->HasOneRef());
}

TEST_F(TraceTokenBufferUnittest, TrackEventRecordBeginCommitAbort) {
  // A record too big for a chunk is refused and needs neither Commit nor
  // Abort.
  EXPECT_EQ(store.BeginTrackEventRecord(BumpAllocator::kChunkSize), nullptr);

  // Fills the first chunk but for 16 bytes, so the reservation below opens a
  // second chunk.
  TraceBlobView filler(PatternBlob(BumpAllocator::kChunkSize - 32));
  TraceTokenBuffer::Id filler_id =
      store.Append(TracePacketData{filler.copy(), state.get()});
  ASSERT_NE(store.BeginTrackEventRecord(1000), nullptr);
  store.AbortTrackEventRecord();

  // The aborted reservation left the second chunk empty, so the record lands
  // at its start.
  RefPtr<PacketSequenceStateGeneration> other =
      PacketSequenceStateGeneration::CreateFirst(
          &context, /*trusted_packet_sequence_id=*/2);
  TraceBlob pattern = PatternBlob(100);
  uint8_t* out = store.BeginTrackEventRecord(1000);
  ASSERT_NE(out, nullptr);
  memcpy(out, pattern.data(), pattern.size());
  TraceTokenBuffer::Id id = store.CommitTrackEventRecord(100, other.get());
  EXPECT_EQ(id.alloc_id.chunk_index, filler_id.alloc_id.chunk_index + 1);
  EXPECT_EQ(id.alloc_id.chunk_offset, 0u);

  // The reservation beyond the record is given back.
  TraceBlobView tbv(PatternBlob(64));
  TraceTokenBuffer::Id next =
      store.Append(TrackEventData(tbv.copy(), other.get(), std::nullopt));
  EXPECT_EQ(next.alloc_id.chunk_index, id.alloc_id.chunk_index);
  EXPECT_EQ(next.alloc_id.chunk_offset, id.alloc_id.chunk_offset + 16u + 104u);

  TracePacketData filler_out = store.Extract<TracePacketData>(filler_id);
  EXPECT_TRUE(SameBytes(filler_out.packet, filler));

  TrackEventData record = store.Extract<TrackEventData>(id);
  EXPECT_TRUE(record.is_record);
  ASSERT_EQ(record.event.size(), 100u);
  EXPECT_EQ(memcmp(record.event.data(), pattern.data(), 100), 0);
  EXPECT_EQ(record.sequence_state, other.get());
  EXPECT_EQ(record.packet, std::nullopt);
  EXPECT_EQ(record.thread_timestamp, std::nullopt);
  EXPECT_EQ(record.thread_instruction_count, std::nullopt);
  EXPECT_DOUBLE_EQ(record.counter_value, 0.0);

  TrackEventData event = store.Extract<TrackEventData>(next);
  EXPECT_FALSE(event.is_record);
  EXPECT_TRUE(SameBytes(event.event, tbv));

  store.FreeMemory();
  EXPECT_TRUE(state->HasOneRef());
  EXPECT_TRUE(other->HasOneRef());

  // The buffer keeps working after its chunks are freed.
  ASSERT_NE(store.BeginTrackEventRecord(64), nullptr);
  TraceTokenBuffer::Id last = store.CommitTrackEventRecord(8, state.get());
  TrackEventData last_record = store.Extract<TrackEventData>(last);
  EXPECT_TRUE(last_record.is_record);
  EXPECT_EQ(last_record.sequence_state, state.get());
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
