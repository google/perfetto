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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_EXTENSION_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_EXTENSION_PARSER_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/proto/typed_proto_field.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

struct TrackEventExtensionParserContext;
class PacketSequenceStateGeneration;

// A single field of a TrackEvent handed to a parser: an in-tree field that
// is imported as args rather than into the row, or an out-of-tree extension
// (`extensions 1000 to 9999`). Decode it with the generated field metadata,
// e.g. field.Cast<FrameworksBaseTrackEvent::kProcessStart>().
using TrackEventExtensionField = TypedProtoField;

// The event a field belongs to, as far as the importer has got with it.
// Built once per event by the importer and handed to every parser.
struct TrackEventFieldContext {
  enum class RowKind : uint8_t { kNone, kSlice, kCounter, kState };
  static constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

  int64_t ts = 0;
  PacketSequenceStateGeneration* sequence_state = nullptr;
  // kNone when the event has no thread or process association.
  UniqueTid utid = kNone;
  UniquePid upid = kNone;

  // The row the event was imported as. kNone for a legacy event imported
  // into the raw table.
  RowKind row_kind = RowKind::kNone;
  uint32_t row_id = kNone;

  // Args of that row, or null when the row takes none.
  ArgsTracker::BoundInserter* args = nullptr;

  bool has_utid() const { return utid != kNone; }
  bool has_upid() const { return upid != kNone; }
  SliceId slice_id() const {
    PERFETTO_DCHECK(row_kind == RowKind::kSlice);
    return SliceId(row_id);
  }
  CounterId counter_id() const {
    PERFETTO_DCHECK(row_kind == RowKind::kCounter);
    return CounterId(row_id);
  }
  StateId state_id() const {
    PERFETTO_DCHECK(row_kind == RowKind::kState);
    return StateId(row_id);
  }
};

// Parses the TrackEvent fields it registered for. Every field of an event
// that the importer does not consume for the row itself is dispatched, in
// order, to the parser registered for its id and then reflected into the
// args table unless the parser claimed it. In-tree fields such as
// task_execution and out-of-tree extensions are handled alike; plugins
// register theirs via RegisterTrackEventExtensions.
//
// Each field id is owned by exactly one parser. Dispatch happens after the
// row has been inserted, so the parser receives its id.
class TrackEventExtensionParser {
 public:
  // kHandled skips the reflection of the field into the args table;
  // kIgnored leaves it to happen.
  enum class Result { kHandled, kIgnored };

  explicit TrackEventExtensionParser(TrackEventExtensionParserContext* context);
  virtual ~TrackEventExtensionParser();

  virtual Result OnTrackEventField(const TrackEventExtensionField& field,
                                   const TrackEventFieldContext& event);

 protected:
  // Registers this parser as the owner of |field_id| (CHECKs if already
  // owned).
  void RegisterTrackEventExtension(uint32_t field_id);

  TrackEventExtensionParserContext* context_;
};

// Per-trace registry of TrackEvent field parsers, mirroring
// ProtoImporterModuleContext: |parsers| owns them and |parsers_by_field| maps
// each registered field id to its owner.
struct TrackEventExtensionParserContext {
  std::vector<std::unique_ptr<TrackEventExtensionParser>> parsers;
  base::FlatHashMap<uint32_t, TrackEventExtensionParser*> parsers_by_field;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_EXTENSION_PARSER_H_
