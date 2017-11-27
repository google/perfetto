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

#include "proto_translation_table.h"

#include "ftrace_procfs.h"
#include "ftrace_reader/format_parser.h"
#include "ftrace_reader/ftrace_to_proto.h"

#include "protos/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto {

namespace {

#define MAX_FIELD_LENGTH 127
#define STRINGIFY(x) STRINGIFY2(x)
#define STRINGIFY2(x) #x

using Event = ProtoTranslationTable::Event;
const std::vector<Event> BuildEventsVector(const std::vector<Event>& events) {
  size_t largest_id = 0;
  for (const Event& event : events) {
    if (event.ftrace_event_id > largest_id)
      largest_id = event.ftrace_event_id;
  }
  std::vector<ProtoTranslationTable::Event> events_by_id;
  events_by_id.resize(largest_id + 1);
  for (const Event& event : events) {
    events_by_id[event.ftrace_event_id] = event;
  }
  events_by_id.shrink_to_fit();
  return events_by_id;
}

}  // namespace

// static
std::unique_ptr<ProtoTranslationTable> ProtoTranslationTable::Create(
    const FtraceProcfs* ftrace_procfs) {
  std::vector<Event> events;
  std::vector<Field> common_fields;

  std::string available = ftrace_procfs->ReadAvailableEvents();
  if (available == "") {
    PERFETTO_DLOG("Could not read available_events");
    return nullptr;
  }
  {
    std::unique_ptr<char[], base::FreeDeleter> copy(strdup(available.c_str()));
    char group_buffer[MAX_FIELD_LENGTH + 1];
    char name_buffer[MAX_FIELD_LENGTH + 1];
    char* s = copy.get();
    for (char* line = strtok(s, "\n"); line; line = strtok(nullptr, "\n")) {
      if (sscanf(line,
                 "%" STRINGIFY(MAX_FIELD_LENGTH) "[^:]:%" STRINGIFY(
                     MAX_FIELD_LENGTH) "s",
                 group_buffer, name_buffer) == 2) {
        std::string name = std::string(name_buffer);
        std::string group = std::string(group_buffer);
        events.emplace_back(Event{name, group});
      }
    }
  }

  // TODO(b/69662589): Hack to get around events missing from available_events.
  events.emplace_back(Event{"print", "ftrace"});

  for (Event& event : events) {
    std::string contents =
        ftrace_procfs->ReadEventFormat(event.group, event.name);
    FtraceEvent ftrace_event;
    if (contents == "" || !ParseFtraceEvent(contents, &ftrace_event)) {
      PERFETTO_DLOG("Could not read '%s'", event.name.c_str());
      continue;
    }
    event.ftrace_event_id = ftrace_event.id;
    event.fields.reserve(ftrace_event.fields.size());
    for (FtraceEvent::Field ftrace_field : ftrace_event.fields) {
      event.fields.push_back(Field{ftrace_field.offset, ftrace_field.size});
    }

    if (common_fields.empty()) {
      for (const FtraceEvent::Field& ftrace_field :
           ftrace_event.common_fields) {
        common_fields.push_back(Field{ftrace_field.offset, ftrace_field.size});
      }
    }
  }

  auto table = std::unique_ptr<ProtoTranslationTable>(
      new ProtoTranslationTable(events, std::move(common_fields)));
  return table;
}

ProtoTranslationTable::ProtoTranslationTable(const std::vector<Event>& events,
                                             std::vector<Field> common_fields)
    : events_(BuildEventsVector(events)),
      largest_id_(events_.size() - 1),
      common_fields_(std::move(common_fields)) {
  for (const Event& event : events) {
    name_to_event_[event.name] = &events_.at(event.ftrace_event_id);
  }
}

ProtoTranslationTable::~ProtoTranslationTable() = default;

}  // namespace perfetto
