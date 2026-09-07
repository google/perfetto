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

#include "src/trace_processor/util/symbolizer/symbolize_database.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/trace_processor/trace_processor.h"
#include "protos/perfetto/trace/interned_data/interned_data.gen.h"
#include "protos/perfetto/trace/profiling/profile_common.gen.h"
#include "protos/perfetto/trace/profiling/profile_packet.gen.h"
#include "protos/perfetto/trace/trace.gen.h"
#include "protos/perfetto/trace/trace_packet.gen.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.gen.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::profiling {
namespace {

void AddProfile(protos::gen::Trace* trace,
                uint32_t sequence_id,
                uint64_t mapping_start,
                uint64_t exact_offset,
                const std::vector<uint64_t>& rel_pcs) {
  auto* intern_packet = trace->add_packet();
  intern_packet->set_trusted_packet_sequence_id(sequence_id);
  intern_packet->set_incremental_state_cleared(true);

  auto* thread = intern_packet->mutable_thread_descriptor();
  thread->set_pid(static_cast<int32_t>(sequence_id));
  thread->set_tid(static_cast<int32_t>(sequence_id));

  auto* interned = intern_packet->mutable_interned_data();
  auto* build_id = interned->add_build_ids();
  build_id->set_iid(1);
  build_id->set_str("build-id");

  auto* mapping = interned->add_mappings();
  mapping->set_iid(1);
  mapping->set_build_id(1);
  mapping->set_start(mapping_start);
  mapping->set_end(mapping_start + 0x10000);
  mapping->set_exact_offset(exact_offset);

  auto* callstack = interned->add_callstacks();
  callstack->set_iid(1);
  for (size_t i = 0; i < rel_pcs.size(); ++i) {
    auto* frame = interned->add_frames();
    frame->set_iid(i + 1);
    frame->set_mapping_id(1);
    frame->set_rel_pc(rel_pcs[i]);
    callstack->add_frame_ids(i + 1);
  }

  auto* sample_packet = trace->add_packet();
  sample_packet->set_trusted_packet_sequence_id(sequence_id);
  auto* samples = sample_packet->mutable_streaming_profile_packet();
  samples->add_callstack_iid(1);
  samples->add_timestamp_delta_us(1);
}

std::unique_ptr<trace_processor::TraceProcessor> LoadTrace(
    const protos::gen::Trace& trace) {
  auto tp = trace_processor::TraceProcessor::CreateInstance({});
  std::string serialized = trace.SerializeAsString();
  std::unique_ptr<uint8_t[]> data(new uint8_t[serialized.size()]);
  memcpy(data.get(), serialized.data(), serialized.size());
  PERFETTO_CHECK(tp->Parse(std::move(data), serialized.size()).ok());
  tp->NotifyEndOfFile();
  return tp;
}

TEST(SymbolizeDatabaseTest, CoalescesEquivalentMappingsAndAddresses) {
  protos::gen::Trace trace;
  AddProfile(&trace, 1, 0x100000, 0, {0x10, 0x20});
  AddProfile(&trace, 2, 0x200000, 0, {0x10, 0x20});

  auto tp = LoadTrace(trace);
  std::vector<UnsymbolizedFrames> frames = CollectUnsymbolizedFrames(tp.get());

  ASSERT_EQ(frames.size(), 1u);
  EXPECT_THAT(frames[0].rel_pcs, testing::ElementsAre(0x10u, 0x20u));
  EXPECT_EQ(frames[0].frame_count, 4u);
}

TEST(SymbolizeDatabaseTest, KeepsAddressCorrectionGroupsSeparate) {
  protos::gen::Trace trace;
  AddProfile(&trace, 1, 0x100000, 0, {0x10, 0x20});
  AddProfile(&trace, 2, 0x200000, 0x1000, {0x10, 0x20});

  auto tp = LoadTrace(trace);
  std::vector<UnsymbolizedFrames> frames = CollectUnsymbolizedFrames(tp.get());

  ASSERT_EQ(frames.size(), 2u);
  EXPECT_THAT(frames[0].rel_pcs, testing::ElementsAre(0x10u, 0x20u));
  EXPECT_THAT(frames[1].rel_pcs, testing::ElementsAre(0x10u, 0x20u));
  EXPECT_EQ(frames[0].frame_count, 2u);
  EXPECT_EQ(frames[1].frame_count, 2u);
}

}  // namespace
}  // namespace perfetto::profiling
