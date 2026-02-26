/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/export/export_arrow.h"

#include <cstdint>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/arrow_ipc.h"
#include "src/trace_processor/core/dataframe/dataframe.h"

namespace perfetto::trace_processor {

base::StatusOr<std::vector<uint8_t>> SerializeTableToArrow(
    const dataframe::Dataframe& df,
    StringPool* pool) {
  std::vector<uint8_t> out;
  RETURN_IF_ERROR(dataframe::SerializeToArrowIpc(
      df, pool, [&out](const uint8_t* data, size_t len) {
        out.insert(out.end(), data, data + len);
      }));
  return std::move(out);
}

}  // namespace perfetto::trace_processor
