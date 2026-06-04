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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_FRAMEWORK_TRACK_EVENT_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_FRAMEWORK_TRACK_EVENT_PARSER_H_

#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/proto/track_event_parser.h"
#include "src/trace_processor/importers/proto/track_event_plugin.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Records AndroidProcessStartEvent and AndroidBinderDiedEvent into
// __intrinsic_android_track_event_process (upid, start_ts, end_ts).
class AndroidFrameworkTrackEventParser
    : public TrackEventPluginRegistry::Plugin {
 public:
  static void Register(TraceProcessorContext* context,
                       TrackEventParser* parser);

  explicit AndroidFrameworkTrackEventParser(TraceProcessorContext* context);
  ~AndroidFrameworkTrackEventParser() override;

 private:
  void HandleProcessStart(protozero::ConstBytes data, int64_t ts);
  void HandleBinderDied(protozero::ConstBytes data, int64_t ts);
  tables::AndroidTrackEventProcessTable::RowReference RowFor(int64_t pid);

  TraceProcessorContext* context_;
  base::FlatHashMap<UniquePid, tables::AndroidTrackEventProcessTable::Id>
      upid_to_row_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_ANDROID_FRAMEWORK_TRACK_EVENT_PARSER_H_
