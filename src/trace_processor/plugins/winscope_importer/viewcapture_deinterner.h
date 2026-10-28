/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_WINSCOPE_IMPORTER_VIEWCAPTURE_DEINTERNER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_WINSCOPE_IMPORTER_VIEWCAPTURE_DEINTERNER_H_

#include <cstdint>
#include <optional>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/arg_sink.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/winscope_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
class PacketSequenceStateGeneration;
}

namespace perfetto::trace_processor::winscope {

// Resolves the interned strings of a ViewCapture snapshot or view, for use as
// a DeinterningArgSink::Lookup. Records every string it resolves, and copies
// the ones that have a column into the snapshot or view row.
class ViewCaptureDeinterner {
 public:
  using IidToStringMap = base::FlatHashMap<uint64_t, StringId>;

  ViewCaptureDeinterner(TraceProcessorContext& context,
                        PacketSequenceStateGeneration* sequence_state,
                        tables::ViewCaptureTable::RowReference* snapshot_row,
                        tables::ViewCaptureViewTable::RowReference* view_row);

  std::optional<base::StringView> Lookup(ArgKey key, uint64_t iid);

  base::FlatHashMap<StringId, IidToStringMap> flat_key_to_iid_args;

 private:
  std::optional<base::StringView> TryDeinternString(base::StringView key,
                                                    uint64_t iid);

  template <uint32_t FieldNumber>
  std::optional<base::StringView> DeinternString(uint64_t);

  template <uint32_t FieldNumber, typename RowRef>
  std::optional<base::StringView> DeinternString(
      uint64_t,
      RowRef*,
      void (RowRef::*setter)(StringPool::Id));

  TraceProcessorContext& context_;
  PacketSequenceStateGeneration* const sequence_state_;
  tables::ViewCaptureTable::RowReference* snapshot_row_;
  tables::ViewCaptureViewTable::RowReference* view_row_;
};

}  // namespace perfetto::trace_processor::winscope

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_WINSCOPE_IMPORTER_VIEWCAPTURE_DEINTERNER_H_
