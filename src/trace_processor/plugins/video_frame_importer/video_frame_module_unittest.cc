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
#include "src/trace_processor/importers/common/global_args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/plugins/video_frame_importer/tables_py.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/frameworks_native_trace_packet.pbzero.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::com::android::internal::pbzero::FrameworksNativeTracePacket;
using ::com::android::internal::pbzero::VideoFrame;
using ::com::android::internal::pbzero::VirtualDisplayComposite;
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
    // The cap path records to the import logs, which interns args.
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, TraceId(1));
    // The table + au_data vector are owned by the plugin in production; the
    // test owns them directly and passes them to the module.
    table_ = std::make_unique<tables::AndroidVideoFramesTable>(
        context_.storage->mutable_string_pool());
  }

  // Feeds one VideoFrame access unit of `au_size` bytes on `display_id` to the
  // module exactly as the proto reader would: a TraceBlobView over the
  // serialized packet, with the VideoFrame written as the frameworks_base
  // TracePacket extension (field 1000) and au_data pointing into that blob.
  void PushAccessUnit(VideoFrameModule& module,
                      uint32_t display_id,
                      size_t au_size,
                      int64_t ts,
                      std::optional<uint64_t> pts_us = std::nullopt) {
    protozero::HeapBuffered<VideoFrame> vf;
    vf->set_display_id(display_id);
    if (pts_us.has_value()) {
      vf->set_pts_us(*pts_us);
    }
    std::vector<uint8_t> au(au_size, 0xAB);
    vf->set_au_data(au.data(), au.size());
    std::vector<uint8_t> vf_bytes = vf.SerializeAsArray();

    protozero::HeapBuffered<TracePacket> packet;
    packet->AppendBytes(FrameworksBaseTracePacket::kVideoFrameFieldNumber,
                        vf_bytes.data(), vf_bytes.size());
    std::vector<uint8_t> bytes = packet.SerializeAsArray();

    TraceBlobView tbv(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
    TracePacketData tpd{tbv.copy(), {}};
    SelectiveTracePacketDecoder decoder(tbv.data(), tbv.length());
    TracePacketField field = decoder.FindUnknownField(
        FrameworksBaseTracePacket::kVideoFrameFieldNumber);
    ASSERT_TRUE(field.valid());
    module.ParseField({decoder, ts, tpd, field});
  }

  // Feeds one VirtualDisplayComposite, written as the frameworks_native
  // TracePacket extension (field 1100), to the module.
  void PushVirtualDisplayComposite(VideoFrameModule& module,
                                   int64_t present_time_us,
                                   int64_t vsync_id) {
    protozero::HeapBuffered<VirtualDisplayComposite> vdc;
    vdc->set_present_time_us(present_time_us);
    vdc->set_vsync_id(vsync_id);
    std::vector<uint8_t> vdc_bytes = vdc.SerializeAsArray();

    protozero::HeapBuffered<TracePacket> packet;
    packet->AppendBytes(
        FrameworksNativeTracePacket::kVirtualDisplayCompositeFieldNumber,
        vdc_bytes.data(), vdc_bytes.size());
    std::vector<uint8_t> bytes = packet.SerializeAsArray();

    TraceBlobView tbv(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
    TracePacketData tpd{tbv.copy(), {}};
    SelectiveTracePacketDecoder decoder(tbv.data(), tbv.length());
    TracePacketField field = decoder.FindUnknownField(
        FrameworksNativeTracePacket::kVirtualDisplayCompositeFieldNumber);
    ASSERT_TRUE(field.valid());
    module.ParseField({decoder, /*ts=*/0, tpd, field});
  }

  uint32_t FrameRowCount() { return table_->row_count(); }

  std::optional<int64_t> VsyncId(uint32_t row) {
    return (*table_)[row].frame_timeline_vsync_id();
  }

  // Number of streams that hit the parse-time cap (the stat is a single
  // counter; the affected display is recorded as an import-log arg).
  int64_t SizeCapHits() {
    return context_.stats_tracker->GetStats(
        stats::android_video_parse_size_cap_hit);
  }

  int64_t UnmatchedComposites() {
    return context_.stats_tracker->GetStats(
        stats::android_video_vsync_composite_unmatched);
  }

  int64_t AmbiguousPresentTimes() {
    return context_.stats_tracker->GetStats(
        stats::android_video_vsync_present_time_ambiguous);
  }

  // A 100-byte cap exercises the drop path without 256 MB of data.
  std::unique_ptr<VideoFrameModule> MakeModule() {
    // module_context can be a throwaway local: RegisterForField only
    // appends to its modules_by_field vector.
    auto module = std::make_unique<VideoFrameModule>(
        &module_context_, &context_, table_.get(), &au_data_);
    module->SetMaxStreamSizeBytesForTesting(100);
    return module;
  }

 protected:
  ProtoImporterModuleContext module_context_;
  TraceProcessorContext context_;
  std::unique_ptr<tables::AndroidVideoFramesTable> table_;
  std::vector<TraceBlobView> au_data_;
};

TEST_F(VideoFrameModuleTest, AdmitsFramesUnderCap) {
  auto module = MakeModule();
  // Three 30-byte AUs (90 total) all fit under the 100-byte cap.
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/2000);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/3000);

  EXPECT_EQ(FrameRowCount(), 3u);
  EXPECT_EQ(SizeCapHits(), 0);
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
  EXPECT_EQ(SizeCapHits(), 1);
}

TEST_F(VideoFrameModuleTest, CapIsPerStream) {
  auto module = MakeModule();
  // Display 0 overflows; display 1 stays under and is unaffected.
  for (int i = 0; i < 6; i++) {
    PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/i + 1);
  }
  PushAccessUnit(*module, /*display_id=*/1, /*au_size=*/30, /*ts=*/100);
  PushAccessUnit(*module, /*display_id=*/1, /*au_size=*/30, /*ts=*/200);

  // 3 from display 0 + 2 from display 1: display 1 is untouched by display 0's
  // overflow, and exactly one stream hit the cap.
  EXPECT_EQ(FrameRowCount(), 5u);
  EXPECT_EQ(SizeCapHits(), 1);
}

TEST_F(VideoFrameModuleTest, LinksFrameTimelineVsyncId) {
  auto module = MakeModule();
  PushVirtualDisplayComposite(*module, /*present_time_us=*/500000,
                              /*vsync_id=*/42);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000,
                 /*pts_us=*/500000);
  module->OnEventsFullyExtracted();

  ASSERT_EQ(FrameRowCount(), 1u);
  EXPECT_EQ(VsyncId(0), 42);
  EXPECT_EQ(UnmatchedComposites(), 0);
  EXPECT_EQ(AmbiguousPresentTimes(), 0);
}

TEST_F(VideoFrameModuleTest, LeavesFrameUnlinkedWithoutMatchingComposite) {
  auto module = MakeModule();
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000,
                 /*pts_us=*/500000);
  // A composite whose present time matches no frame, e.g. one queued to a
  // virtual display that isn't being captured.
  PushVirtualDisplayComposite(*module, /*present_time_us=*/600000,
                              /*vsync_id=*/42);
  module->OnEventsFullyExtracted();

  ASSERT_EQ(FrameRowCount(), 1u);
  EXPECT_EQ(VsyncId(0), std::nullopt);
  EXPECT_EQ(UnmatchedComposites(), 1);
  EXPECT_EQ(AmbiguousPresentTimes(), 0);
}

TEST_F(VideoFrameModuleTest, LinksFrameParsedBeforeComposite) {
  auto module = MakeModule();
  // The backfill runs once all packets are parsed, so a composite that comes
  // after its frame still links.
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000,
                 /*pts_us=*/500000);
  PushVirtualDisplayComposite(*module, /*present_time_us=*/500000,
                              /*vsync_id=*/42);
  module->OnEventsFullyExtracted();

  ASSERT_EQ(FrameRowCount(), 1u);
  EXPECT_EQ(VsyncId(0), 42);
  EXPECT_EQ(UnmatchedComposites(), 0);
  EXPECT_EQ(AmbiguousPresentTimes(), 0);
}

TEST_F(VideoFrameModuleTest, LeavesFrameUnlinkedOnConflictingVsyncIds) {
  auto module = MakeModule();
  // Two composites share a present time but disagree on the vsync id, so
  // either could have produced the frame.
  PushVirtualDisplayComposite(*module, /*present_time_us=*/500000,
                              /*vsync_id=*/42);
  PushVirtualDisplayComposite(*module, /*present_time_us=*/500000,
                              /*vsync_id=*/43);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000,
                 /*pts_us=*/500000);
  module->OnEventsFullyExtracted();

  ASSERT_EQ(FrameRowCount(), 1u);
  EXPECT_EQ(VsyncId(0), std::nullopt);
  EXPECT_EQ(UnmatchedComposites(), 0);
  EXPECT_EQ(AmbiguousPresentTimes(), 1);
}

TEST_F(VideoFrameModuleTest, LinksRepeatedPresentTimeWithSameVsyncId) {
  auto module = MakeModule();
  // Two virtual displays composited in the same frame share its vsync id, so
  // a repeated present time with the same vsync id is not ambiguous.
  PushVirtualDisplayComposite(*module, /*present_time_us=*/500000,
                              /*vsync_id=*/42);
  PushVirtualDisplayComposite(*module, /*present_time_us=*/500000,
                              /*vsync_id=*/42);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000,
                 /*pts_us=*/500000);
  module->OnEventsFullyExtracted();

  ASSERT_EQ(FrameRowCount(), 1u);
  EXPECT_EQ(VsyncId(0), 42);
  EXPECT_EQ(UnmatchedComposites(), 0);
  EXPECT_EQ(AmbiguousPresentTimes(), 0);
}

TEST_F(VideoFrameModuleTest, IgnoresCompositeWithInvalidVsyncId) {
  auto module = MakeModule();
  // SurfaceFlinger's INVALID_VSYNC_ID (-1) has no frame timeline to link to,
  // so the composite neither links the frame nor counts as unmatched.
  PushVirtualDisplayComposite(*module, /*present_time_us=*/500000,
                              /*vsync_id=*/-1);
  PushAccessUnit(*module, /*display_id=*/0, /*au_size=*/30, /*ts=*/1000,
                 /*pts_us=*/500000);
  module->OnEventsFullyExtracted();

  ASSERT_EQ(FrameRowCount(), 1u);
  EXPECT_EQ(VsyncId(0), std::nullopt);
  EXPECT_EQ(UnmatchedComposites(), 0);
  EXPECT_EQ(AmbiguousPresentTimes(), 0);
}

}  // namespace
}  // namespace perfetto::trace_processor
