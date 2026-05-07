// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "src/trace_processor/perfetto_sql/intrinsics/functions/video_frame_image.h"

#include <sqlite3.h>

#include "src/trace_processor/sqlite/bindings/sqlite_result.h"
#include "src/trace_processor/sqlite/bindings/sqlite_value.h"

namespace perfetto::trace_processor {

void VideoFrameImageFunction::Step(sqlite3_context* ctx,
                                   int,
                                   sqlite3_value** argv) {
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

}  // namespace perfetto::trace_processor
