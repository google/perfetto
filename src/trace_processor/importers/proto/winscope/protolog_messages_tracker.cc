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

#include "protolog_messages_tracker.h"
#include "perfetto/ext/base/crash_keys.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {
ProtoLogMessagesTracker::ProtoLogMessagesTracker() {}

ProtoLogMessagesTracker::~ProtoLogMessagesTracker() = default;

void ProtoLogMessagesTracker::TrackMessage(
    TrackedProtoLogMessage tracked_protolog_message) {
  tracked_protolog_messages
      .Insert(tracked_protolog_message.message_id,
              std::vector<TrackedProtoLogMessage>())
      .first->emplace_back(tracked_protolog_message);
}

const std::optional<
    std::vector<ProtoLogMessagesTracker::TrackedProtoLogMessage>*>
ProtoLogMessagesTracker::GetTrackedMessagesByMessageId(uint64_t message_id) {
  auto tracked_messages = tracked_protolog_messages.Find(message_id);

  if (tracked_messages == nullptr) {
    // No tracked messages found for this id
    return std::nullopt;
  }

  return tracked_messages;
}

}  // namespace trace_processor
}  // namespace perfetto
