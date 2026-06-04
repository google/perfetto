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

#include "src/trace_processor/plugins/video_frame_importer/video_frame_module.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/android/video_frame.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::perfetto::protos::pbzero::TracePacket;

class VideoFrameModuleTest : public testing::Test {
 public:
  VideoFrameModuleTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    // StatsTracker's ctor reads context_.trace_id(), which needs trace_state.
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
  }

  // Feeds one VideoFrame access unit of `au_size` bytes on `display_id` to
  // the module exactly as the proto reader would: a TraceBlobView over the
  // serialized packet, with au_data pointing into that same blob.
  void PushAccessUnit(VideoFrameModule& module,
                      uint32_t display_id,
                      size_t au_size,
                      int64_t ts) {
    protozero::HeapBuffered<TracePacket> packet;
    auto* vf = packet->set_video_frame();
    vf->set_display_id(display_id);
    std::vector<uint8_t> au(au_size, 0xAB);
    vf->set_au_data(au.data(), au.size());
    std::vector<uint8_t> bytes = packet.SerializeAsArray();

    TraceBlobView tbv(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
    TracePacket::Decoder decoder(tbv.data(), tbv.length());
    TracePacketData tpd{tbv.copy(), {}};
    module.ParseTracePacketData(decoder, ts, tpd,
                                TracePacket::kVideoFrameFieldNumber);
  }

  uint32_t FrameRowCount() {
    return context_.storage->video_frames_table().row_count();
  }

  int64_t SizeCapHits(uint32_t display_id) {
    return context_.stats_tracker
        ->GetIndexedStats(stats::android_video_size_cap_hit,
                          static_cast<int>(display_id))
        .value_or(0);
  }

  // A 100-byte cap exercises the drop path without 256 MB of data.
  std::unique_ptr<VideoFrameModule> MakeModule() {
    // module_context can be a throwaway local: RegisterForField only
    // appends to its modules_by_field vector.
    auto module =
        std::make_unique<VideoFrameModule>(&module_context_, &context_);
    module->SetMaxStreamSizeBytesForTesting(100);
    return module;
  }

 protected:
  ProtoImporterModuleContext module_context_;
  TraceProcessorContext context_;
};

TEST_F(VideoFrameModuleTest, AdmitsFramesUnderCap) {
  auto module = MakeModule();
  // Three 30-byte AUs (90 total) all fit under the 100-byte cap.
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/2000);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/3000);

  EXPECT_EQ(FrameRowCount(), 3u);
  EXPECT_EQ(SizeCapHits(0), 0);
}

TEST_F(VideoFrameModuleTest, DropsFramesPastCapAndReportsOnce) {
  auto module = MakeModule();
  // 30-byte AUs: the 4th would reach 120 > 100, so it and everything after
  // are dropped, and the size-cap stat is set exactly once.
  for (int i = 0; i < 6; i++) {
    PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30,
                   /*ts=*/1000 * (i + 1));
  }

  EXPECT_EQ(FrameRowCount(), 3u);
  EXPECT_EQ(SizeCapHits(0), 1);
}

TEST_F(VideoFrameModuleTest, CapIsPerStream) {
  auto module = MakeModule();
  // Display 0 overflows; display 1 stays under and is unaffected.
  for (int i = 0; i < 6; i++) {
    PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/i + 1);
  }
  PushAccessUnit(*module, /*display_id=*/1, /*au_size=*/30, /*ts=*/100);
  PushAccessUnit(*module, /*display_id=*/1, /*au_size=*/30, /*ts=*/200);

  EXPECT_EQ(FrameRowCount(), 5u);  // 3 from display 0 + 2 from display 1.
  EXPECT_EQ(SizeCapHits(0), 1);
  EXPECT_EQ(SizeCapHits(1), 0);
}

}  // namespace
}  // namespace perfetto::trace_processor
