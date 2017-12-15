/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/ftrace_reader/event_info.h"

namespace perfetto {
namespace {

Field FieldFromNameIdType(const char* name, size_t id, ProtoFieldType type) {
  Field field{};
  field.ftrace_name = name;
  field.proto_field_id = id;
  field.proto_field_type = type;
  return field;
}

}  // namespace

// TODO(hjd): Auto-generate this file.
// TODO(b/70373826): Reduce runetime overhead with constexpr magic etc.
std::vector<Event> GetStaticEventInfo() {
  std::vector<Event> events;

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "print";
    event->group = "ftrace";
    event->proto_field_id = 3;
    event->fields.push_back(FieldFromNameIdType("buf", 2, kProtoString));
  }

  {
    events.emplace_back(Event{});
    Event* event = &events.back();
    event->name = "sched_switch";
    event->group = "sched";
    event->proto_field_id = 4;
    event->fields.push_back(FieldFromNameIdType("prev_comm", 1, kProtoString));
    event->fields.push_back(FieldFromNameIdType("prev_pid", 2, kProtoInt32));
    event->fields.push_back(FieldFromNameIdType("prev_prio", 3, kProtoInt32));
    event->fields.push_back(FieldFromNameIdType("prev_state", 4, kProtoInt64));
    event->fields.push_back(FieldFromNameIdType("next_comm", 5, kProtoString));
    event->fields.push_back(FieldFromNameIdType("next_pid", 6, kProtoInt32));
    event->fields.push_back(FieldFromNameIdType("next_prio", 7, kProtoInt32));
  }

  return events;
}

std::vector<Field> GetStaticCommonFieldsInfo() {
  std::vector<Field> fields;

  fields.push_back(FieldFromNameIdType("common_pid", 2, kProtoInt32));

  return fields;
}

bool SetTranslationStrategy(FtraceFieldType ftrace,
                            ProtoFieldType proto,
                            TranslationStrategy* out) {
  if (ftrace == kFtraceUint32 && proto == kProtoUint32) {
    *out = kUint32ToUint32;
  } else if (ftrace == kFtraceUint32 && proto == kProtoUint64) {
    *out = kUint32ToUint64;
  } else if (ftrace == kFtraceUint64 && proto == kProtoUint64) {
    *out = kUint64ToUint64;
  } else if (ftrace == kFtraceInt32 && proto == kProtoInt32) {
    *out = kInt32ToInt32;
  } else if (ftrace == kFtraceInt32 && proto == kProtoInt64) {
    *out = kInt32ToInt64;
  } else if (ftrace == kFtraceInt64 && proto == kProtoInt64) {
    *out = kInt64ToInt64;
  } else if (ftrace == kFtraceFixedCString && proto == kProtoString) {
    *out = kFixedCStringToString;
  } else if (ftrace == kFtraceCString && proto == kProtoString) {
    *out = kCStringToString;
  } else {
    PERFETTO_DLOG("No translation strategy for '%s' -> '%s'", ToString(ftrace),
                  ToString(proto));
    return false;
  }
  return true;
}

}  // namespace perfetto
