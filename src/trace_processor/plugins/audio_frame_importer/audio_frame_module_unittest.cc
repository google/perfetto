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

#include "src/trace_processor/plugins/audio_frame_importer/audio_frame_module.h"

#include <cstdint>
#include <memory>
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
#include "src/trace_processor/plugins/audio_frame_importer/tables_py.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor {
namespace {

using ::com::android::internal::pbzero::AudioFrame;
using ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using ::perfetto::protos::pbzero::TracePacket;

class AudioFrameModuleTest : public testing::Test {
 public:
  AudioFrameModuleTest() {
    context_.storage = std::make_unique<TraceStorage>();
    context_.global_stats_tracker =
        std::make_unique<GlobalStatsTracker>(context_.storage.get());
    context_.machine_tracker =
        std::make_unique<MachineTracker>(&context_, kDefaultMachineId);
    context_.trace_state =
        TraceProcessorContextPtr<TraceProcessorContext::TraceState>::MakeRoot(
            TraceProcessorContext::TraceState{TraceId(0)});
    context_.stats_tracker = std::make_unique<StatsTracker>(&context_);
    context_.global_args_tracker =
        std::make_unique<GlobalArgsTracker>(context_.storage.get());
    context_.import_logs_tracker =
        std::make_unique<ImportLogsTracker>(&context_, TraceId(1));
    table_ = std::make_unique<tables::AndroidAudioFramesTable>(
        context_.storage->mutable_string_pool());
  }

  // Feeds one AudioFrame access unit of `au_size` bytes on `stream_id` to the
  // module exactly as the proto reader would.
  void PushAccessUnit(AudioFrameModule& module,
                      uint32_t stream_id,
                      size_t au_size,
                      int64_t ts) {
    protozero::HeapBuffered<AudioFrame> vf;
    vf->set_stream_id(stream_id);
    std::vector<uint8_t> au(au_size, 0xAB);
    vf->set_au_data(au.data(), au.size());
    std::vector<uint8_t> vf_bytes = vf.SerializeAsArray();

    protozero::HeapBuffered<TracePacket> packet;
    packet->AppendBytes(FrameworksBaseTracePacket::kAudioFrameFieldNumber,
                        vf_bytes.data(), vf_bytes.size());
    std::vector<uint8_t> bytes = packet.SerializeAsArray();

    TraceBlobView tbv(TraceBlob::CopyFrom(bytes.data(), bytes.size()));
    TracePacket::Decoder decoder(tbv.data(), tbv.length());
    TracePacketData tpd{tbv.copy(), {}};
    module.ParseTracePacketData(
        decoder, ts, tpd, FrameworksBaseTracePacket::kAudioFrameFieldNumber);
  }

  uint32_t FrameRowCount() { return table_->row_count(); }

  int64_t SizeCapHits() {
    return context_.stats_tracker->GetStats(
        stats::android_audio_parse_size_cap_hit);
  }

  std::unique_ptr<AudioFrameModule> MakeModule() {
    auto module = std::make_unique<AudioFrameModule>(
        &module_context_, &context_, table_.get(), &au_data_);
    module->SetMaxStreamSizeBytesForTesting(100);
    return module;
  }

 protected:
  ProtoImporterModuleContext module_context_;
  TraceProcessorContext context_;
  std::unique_ptr<tables::AndroidAudioFramesTable> table_;
  std::vector<TraceBlobView> au_data_;
};

TEST_F(AudioFrameModuleTest, AdmitsFramesUnderCap) {
  auto module = MakeModule();
  PushAccessUnit(*module, /*stream_id=*/0, /*au_size=*/30, /*ts=*/1000);
  PushAccessUnit(*module, /*stream_id=*/0, /*au_size=*/30, /*ts=*/2000);
  PushAccessUnit(*module, /*stream_id=*/0, /*au_size=*/30, /*ts=*/3000);

  EXPECT_EQ(FrameRowCount(), 3u);
  EXPECT_EQ(SizeCapHits(), 0);
}

TEST_F(AudioFrameModuleTest, DropsFramesPastCapAndReportsOnce) {
  auto module = MakeModule();
  for (int i = 0; i < 6; i++) {
    PushAccessUnit(*module, /*stream_id=*/0, /*au_size=*/30,
                   /*ts=*/1000 * (i + 1));
  }

  EXPECT_EQ(FrameRowCount(), 3u);
  EXPECT_EQ(SizeCapHits(), 1);
}

TEST_F(AudioFrameModuleTest, CapIsPerStream) {
  auto module = MakeModule();
  for (int i = 0; i < 6; i++) {
    PushAccessUnit(*module, /*stream_id=*/0, /*au_size=*/30, /*ts=*/i + 1);
  }
  PushAccessUnit(*module, /*stream_id=*/1, /*au_size=*/30, /*ts=*/100);
  PushAccessUnit(*module, /*stream_id=*/1, /*au_size=*/30, /*ts=*/200);

  EXPECT_EQ(FrameRowCount(), 5u);
  EXPECT_EQ(SizeCapHits(), 1);
}

}  // namespace
}  // namespace perfetto::trace_processor
