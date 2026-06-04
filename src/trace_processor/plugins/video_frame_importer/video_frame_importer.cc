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

#include "src/trace_processor/plugins/video_frame_importer/video_frame_importer.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/plugins/video_frame_importer/video_frame_module.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_type.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::video_frame_importer {
namespace {

// Returns the raw encoded bytes (Annex-B au_data, or codec_config) for one
// row of android_video_frames. The bytes are a zero-copy view into the
// original trace blob; TraceStorage holds the underlying refcount alive, so
// SQLite is given a static pointer.
struct VideoFrameAuData : public sqlite::Function<VideoFrameAuData> {
  static constexpr char kName[] = "__intrinsic_video_frame_au_data";
  static constexpr int kArgCount = 1;
  using UserData = TraceStorage;

  static void Step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    PERFETTO_DCHECK(argc == 1);
    if (sqlite::value::Type(argv[0]) == sqlite::Type::kNull) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }
    TraceStorage* storage = GetUserData(ctx);
    auto id = static_cast<uint32_t>(sqlite::value::Int64(argv[0]));
    const auto& blobs = storage->video_frame_au_data();
    if (id >= blobs.size()) {
      return sqlite::utils::ReturnNullFromFunction(ctx);
    }
    const TraceBlobView& v = blobs[id];
    sqlite::result::StaticBytes(ctx, v.data(), static_cast<int>(v.size()));
  }
};

class VideoFrameImporter : public Plugin<VideoFrameImporter> {
 public:
  ~VideoFrameImporter() override;

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    module_context->modules.emplace_back(
        new VideoFrameModule(module_context, trace_context));
  }

  void RegisterFunctions(PerfettoSqlConnection*,
                         std::vector<FunctionRegistration>& out) override {
    out.push_back(MakeFunctionRegistration<VideoFrameAuData>(
        trace_context_->storage.get()));
  }
};

VideoFrameImporter::~VideoFrameImporter() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<VideoFrameImporter>();
      },
      VideoFrameImporter::kPluginId, VideoFrameImporter::kDepIds.data(),
      VideoFrameImporter::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::video_frame_importer
