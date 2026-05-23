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

#include "src/trace_processor/plugins/video_frame_image/video_frame_image.h"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/perfetto_sql/engine/perfetto_sql_connection.h"
#include "src/trace_processor/sqlite/bindings/sqlite_function.h"
#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {

// video_frame_image(row_id) returns the raw encoded bytes (H.264/HEVC access
// unit, or codec_config for the setup frame) for a row in
// __intrinsic_video_frames. row_id is the table row id == blob store index.
struct VideoFrameImageFunction
    : public sqlite::Function<VideoFrameImageFunction> {
  static constexpr char kName[] = "video_frame_image";
  static constexpr int kArgCount = 1;

  using UserData = TraceStorage;

  static void Step(sqlite3_context* ctx, int, sqlite3_value** argv) {
    auto* storage = sqlite::Function<VideoFrameImageFunction>::GetUserData(ctx);

    if (sqlite::value::IsNull(argv[0])) {
      sqlite::result::Null(ctx);
      return;
    }

    int64_t row_id = sqlite::value::Int64(argv[0]);
    const auto& blobs = storage->video_frame_data();

    if (row_id < 0 || static_cast<size_t>(row_id) >= blobs.size()) {
      sqlite::result::Null(ctx);
      return;
    }

    const auto& blob = blobs[static_cast<size_t>(row_id)];
    if (blob.data() == nullptr || blob.length() == 0) {
      sqlite::result::Null(ctx);
      return;
    }

    sqlite::result::StaticBytes(ctx, blob.data(),
                                static_cast<int>(blob.length()));
  }
};

}  // namespace

namespace video_frame_image {
namespace {

class VideoFrameImagePlugin : public Plugin<VideoFrameImagePlugin> {
 public:
  ~VideoFrameImagePlugin() override;
  void RegisterFunctions(PerfettoSqlConnection*,
                         std::vector<FunctionRegistration>& out) override {
    TraceStorage* storage = trace_context_->storage.get();
    out.push_back(MakeFunctionRegistration<VideoFrameImageFunction>(storage));
  }
};
VideoFrameImagePlugin::~VideoFrameImagePlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<VideoFrameImagePlugin>();
      },
      VideoFrameImagePlugin::kPluginId, VideoFrameImagePlugin::kDepIds.data(),
      VideoFrameImagePlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace video_frame_image

}  // namespace perfetto::trace_processor
