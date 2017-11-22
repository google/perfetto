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

#include <fstream>
#include <sstream>
#include <string>

#include "ftrace_procfs.h"
#include "ftrace_reader/format_parser.h"
#include "ftrace_reader/ftrace_to_proto.h"

namespace perfetto {

namespace {

#define MAX_FIELD_LENGTH 127
#define STRINGIFY(x) STRINGIFY2(x)
#define STRINGIFY2(x) #x

std::string ReadFileIntoString(std::string path) {
  std::ifstream fin(path, std::ios::in);
  if (!fin) {
    return "";
  }
  std::string str;
  fin.seekg(0, std::ios::end);
  str.reserve(fin.tellg());
  fin.seekg(0, std::ios::beg);
  str.assign(std::istreambuf_iterator<char>(fin),
             std::istreambuf_iterator<char>());
  return str;
}

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
    std::string path_to_root,
    const FtraceProcfs* ftrace_procfs) {
  if (path_to_root.length() == 0 || path_to_root.back() != '/') {
    PERFETTO_DLOG("Path '%s' must end with /.", path_to_root.c_str());
    return nullptr;
  }
  std::vector<Event> events;
  std::vector<Field> common_fields;

  std::string available_path = path_to_root + "/available_events";
  std::string available_contents = ReadFileIntoString(available_path);
  if (available_contents == "") {
    PERFETTO_DLOG("Could not read '%s'", available_path.c_str());
    return nullptr;
  }
  {
    std::unique_ptr<char[], base::FreeDeleter> copy(
        strdup(available_contents.c_str()));
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

  for (Event& event : events) {
    std::string path =
        path_to_root + "/events/" + event.group + "/" + event.name + "/format";
    std::string contents = ReadFileIntoString(path);
    FtraceEvent ftrace_event;
    if (contents == "" || !ParseFtraceEvent(contents, &ftrace_event)) {
      PERFETTO_DLOG("Could not read '%s'", path.c_str());
      continue;
    }
    event.ftrace_event_id = ftrace_event.id;
    event.fields.reserve(ftrace_event.fields.size());
    for (FtraceEvent::Field ftrace_field : ftrace_event.fields) {
      event.fields.push_back(Field{ftrace_field.offset, ftrace_field.size});
    }
  }

  if (events.size() == 0) {
    return nullptr;
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
