/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_PROTOLOG_MESSAGES_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_PROTOLOG_MESSAGES_TRACKER_H_

#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

class ProtoLogMessagesTracker : public Destructible {
 public:
  explicit ProtoLogMessagesTracker();
  virtual ~ProtoLogMessagesTracker() override;

  struct TrackedProtoLogMessage {
    uint64_t message_id;
    std::vector<int64_t> sint64_params;
    std::vector<double> double_params;
    std::vector<bool> boolean_params;
    std::vector<std::string> string_params;
    std::optional<StringId> stacktrace;
    tables::ProtoLogTable::Id table_row_id;
    int64_t timestamp;
  };

  static ProtoLogMessagesTracker* GetOrCreate(TraceProcessorContext* context) {
    if (!context->shell_transitions_tracker) {
      context->shell_transitions_tracker.reset(new ProtoLogMessagesTracker());
    }
    return static_cast<ProtoLogMessagesTracker*>(
        context->shell_transitions_tracker.get());
  }

  void TrackMessage(TrackedProtoLogMessage tracked_protolog_message);
  const std::optional<
      std::vector<ProtoLogMessagesTracker::TrackedProtoLogMessage>*>
  GetTrackedMessagesByMessageId(uint64_t message_id);

 private:
  base::FlatHashMap<uint64_t, std::vector<TrackedProtoLogMessage>>
      tracked_protolog_messages;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_WINSCOPE_PROTOLOG_MESSAGES_TRACKER_H_
