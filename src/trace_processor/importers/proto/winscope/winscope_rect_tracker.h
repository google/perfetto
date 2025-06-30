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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_RECT_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_RECT_TRACKER_H_

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "src/trace_processor/importers/proto/winscope/winscope_geometry.h"
#include "src/trace_processor/tables/winscope_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::winscope {

struct RectHasher {
  size_t operator()(const geometry::Rect& r) const {
    perfetto::base::Hasher hasher;
    hasher.Update(r.x);
    hasher.Update(r.y);
    hasher.Update(r.w);
    hasher.Update(r.h);
    return static_cast<size_t>(hasher.digest());
  }
};

class WinscopeRectTracker {
 public:
  explicit WinscopeRectTracker(TraceProcessorContext* context)
      : context_(context) {}
  TraceProcessorContext* context_;

  const tables::WinscopeRectTable::Id& GetOrInsertRow(geometry::Rect& rect);

 private:
  base::FlatHashMap<geometry::Rect, tables::WinscopeRectTable::Id, RectHasher>
      rows_;
};

}  // namespace perfetto::trace_processor::winscope

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_WINSCOPE_RECT_TRACKER_H_
