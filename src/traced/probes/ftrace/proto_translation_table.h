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

#ifndef SRC_TRACED_PROBES_FTRACE_PROTO_TRANSLATION_TABLE_H_
#define SRC_TRACED_PROBES_FTRACE_PROTO_TRANSLATION_TABLE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "perfetto/base/scoped_file.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/format_parser.h"

namespace perfetto {

class FtraceProcfs;

namespace protos {
namespace pbzero {
class FtraceEventBundle;
}  // namespace pbzero
}  // namespace protos

bool InferFtraceType(const std::string& type_and_name,
                     size_t size,
                     bool is_signed,
                     FtraceFieldType* out);

class ProtoTranslationTable {
 public:
  struct FtracePageHeaderSpec {
    FtraceEvent::Field timestamp{};
    FtraceEvent::Field overwrite{};
    FtraceEvent::Field size{};
  };

  static FtracePageHeaderSpec DefaultPageHeaderSpecForTesting();

  // This method mutates the |events| and |common_fields| vectors to
  // fill some of the fields and to delete unused events/fields
  // before std:move'ing them into the ProtoTranslationTable.
  static std::unique_ptr<ProtoTranslationTable> Create(
      const FtraceProcfs* ftrace_procfs,
      std::vector<Event> events,
      std::vector<Field> common_fields);
  ~ProtoTranslationTable();

  ProtoTranslationTable(const std::vector<Event>& events,
                        std::vector<Field> common_fields,
                        FtracePageHeaderSpec ftrace_page_header_spec);

  size_t largest_id() const { return largest_id_; }

  const std::vector<Field>& common_fields() const { return common_fields_; }

  const Event* GetEventByName(const std::string& name) const {
    if (!name_to_event_.count(name))
      return nullptr;
    return name_to_event_.at(name);
  }

  const std::vector<const Event*>* GetEventsByGroup(
      const std::string& group) const {
    if (!group_to_events_.count(group))
      return nullptr;
    return &group_to_events_.at(group);
  }

  const Event* GetEventById(size_t id) const {
    if (id == 0 || id > largest_id_)
      return nullptr;
    if (!events_.at(id).ftrace_event_id)
      return nullptr;
    return &events_.at(id);
  }

  size_t EventNameToFtraceId(const std::string& name) const {
    if (!name_to_event_.count(name))
      return 0;
    return name_to_event_.at(name)->ftrace_event_id;
  }

  const std::vector<Event>& events() { return events_; }
  const FtracePageHeaderSpec& ftrace_page_header_spec() const {
    return ftrace_page_header_spec_;
  }

 private:
  ProtoTranslationTable(const ProtoTranslationTable&) = delete;
  ProtoTranslationTable& operator=(const ProtoTranslationTable&) = delete;

  const std::vector<Event> events_;
  size_t largest_id_;
  std::map<std::string, const Event*> name_to_event_;
  std::map<std::string, std::vector<const Event*>> group_to_events_;
  std::vector<Field> common_fields_;
  FtracePageHeaderSpec ftrace_page_header_spec_{};
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_PROTO_TRANSLATION_TABLE_H_
