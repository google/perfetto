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

#include <optional>

#include "perfetto/base/compiler.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

class TraceTokenBufferUnittest : public testing::Test {
 protected:
  TraceTokenBuffer store;
  TraceProcessorContext context;
  RefPtr<PacketSequenceStateGeneration> state =
      PacketSequenceStateGeneration::CreateFirst(&context);
};

TEST_F(TraceTokenBufferUnittest, TracePacketDataInOut) {
  TraceBlobView tbv(TraceBlob::Allocate(1024));
  TracePacketData tpd{tbv.copy(), state};

  TraceTokenBuffer::Id id = store.Append(std::move(tpd));
  TracePacketData extracted = store.Extract<TracePacketData>(id);
  ASSERT_EQ(extracted.packet, tbv);
  ASSERT_EQ(extracted.sequence_state, state);
}

TEST_F(TraceTokenBufferUnittest, PacketAppendMultipleBlobs) {
  TraceBlobView tbv_1(TraceBlob::Allocate(1024));
  TraceBlobView tbv_2(TraceBlob::Allocate(2048));
  TraceBlobView tbv_3(TraceBlob::Allocate(4096));

  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{tbv_1.copy(), state});
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{tbv_2.copy(), state});
  ASSERT_EQ(store.Extract<TracePacketData>(id_1).packet, tbv_1);
  ASSERT_EQ(store.Extract<TracePacketData>(id_2).packet, tbv_2);

  TraceTokenBuffer::Id id_3 =
      store.Append(TracePacketData{tbv_3.copy(), state});
  ASSERT_EQ(store.Extract<TracePacketData>(id_3).packet, tbv_3);
}

TEST_F(TraceTokenBufferUnittest, BlobSharing) {
  TraceBlobView root(TraceBlob::Allocate(2048));
  TraceBlobView tbv_1 = root.slice_off(0, 1024);
  TraceBlobView tbv_2 = root.slice_off(1024, 512);
  TraceBlobView tbv_3 = root.slice_off(1536, 512);

  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{tbv_1.copy(), state});
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{tbv_2.copy(), state});
  ASSERT_EQ(store.Extract<TracePacketData>(id_1).packet, tbv_1);
  ASSERT_EQ(store.Extract<TracePacketData>(id_2).packet, tbv_2);

  TraceTokenBuffer::Id id_3 =
      store.Append(TracePacketData{tbv_3.copy(), state});
  ASSERT_EQ(store.Extract<TracePacketData>(id_3).packet, tbv_3);
}

TEST_F(TraceTokenBufferUnittest, SequenceStateSharing) {
  TraceBlobView root(TraceBlob::Allocate(2048));
  TraceBlobView tbv_1 = root.slice_off(0, 1024);
  TraceBlobView tbv_2 = root.slice_off(1024, 512);

  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{tbv_1.copy(), state});
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{tbv_2.copy(), state});
  ASSERT_EQ(store.Extract<TracePacketData>(id_1).sequence_state, state);
  ASSERT_EQ(store.Extract<TracePacketData>(id_2).sequence_state, state);
}

TEST_F(TraceTokenBufferUnittest, ManySequenceState) {
  TraceBlobView root(TraceBlob::Allocate(1024));

  std::array<TraceTokenBuffer::Id, 1024> ids;
  std::array<PacketSequenceStateGeneration*, 1024> refs;
  for (uint32_t i = 0; i < 1024; ++i) {
    refs[i] = state.get();
    ids[i] = store.Append(TracePacketData{root.slice_off(i, 1), state});
    state = state->OnNewTracePacketDefaults(TraceBlobView());
  }

  for (uint32_t i = 0; i < 1024; ++i) {
    ASSERT_EQ(refs[i],
              store.Extract<TracePacketData>(ids[i]).sequence_state.get());
  }
}

TEST_F(TraceTokenBufferUnittest, PacketLargeOffset) {
  TraceBlobView tbv(TraceBlob::Allocate(256ul * 1024));

  TraceBlobView slice_1 = tbv.slice_off(0, 1024ul);
  TraceTokenBuffer::Id id_1 =
      store.Append(TracePacketData{slice_1.copy(), state});
  TracePacketData out_1 = store.Extract<TracePacketData>(id_1);
  ASSERT_EQ(out_1.packet, slice_1);
  ASSERT_EQ(out_1.sequence_state, state);

  TraceBlobView slice_2 = tbv.slice_off(128ul * 1024, 1024ul);
  TraceTokenBuffer::Id id_2 =
      store.Append(TracePacketData{slice_2.copy(), state});
  TracePacketData out_2 = store.Extract<TracePacketData>(id_2);
  ASSERT_EQ(out_2.packet, slice_2);
  ASSERT_EQ(out_2.sequence_state, state);
}

TEST_F(TraceTokenBufferUnittest, TrackEventDataInOut) {
  TraceBlobView tbv(TraceBlob::Allocate(1234));
  TrackEventData ted(tbv.copy(), state);
  ted.thread_instruction_count = 123;
  ted.extra_counter_values = {10, 2, 0, 0, 0, 0, 0, 0};
  auto counter_array = ted.extra_counter_values;

  TraceTokenBuffer::Id id = store.Append(std::move(ted));
  TrackEventData extracted = store.Extract<TrackEventData>(id);
  ASSERT_EQ(extracted.trace_packet_data.packet, tbv);
  ASSERT_EQ(extracted.trace_packet_data.sequence_state, state);
  ASSERT_EQ(extracted.thread_instruction_count, 123);
  ASSERT_EQ(extracted.thread_timestamp, std::nullopt);
  ASSERT_DOUBLE_EQ(extracted.counter_value, 0.0);
  ASSERT_EQ(extracted.extra_counter_values, counter_array);
}

TEST_F(TraceTokenBufferUnittest, ExtractOrAppendAfterFreeMemory) {
  auto unused_res = store.Extract<TraceBlobView>(
      store.Append(TraceBlobView(TraceBlob::Allocate(1234))));
  base::ignore_result(unused_res);

  store.FreeMemory();

  TraceTokenBuffer::Id id =
      store.Append(TraceBlobView(TraceBlob::Allocate(4567)));
  TraceBlobView tbv = store.Extract<TraceBlobView>(id);
  ASSERT_EQ(tbv.size(), 4567u);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
