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

#ifndef FTRACE_PROTO_TRANSLATION_TABLE_H_
#define FTRACE_PROTO_TRANSLATION_TABLE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "perfetto_base/scoped_file.h"

namespace perfetto {

class FtraceProcfs;

namespace protos {
namespace pbzero {
class FtraceEventBundle;
}  // namespace pbzero
}  // namespace protos

class ProtoTranslationTable {
 public:
  enum FtraceFieldType {
    kFtraceNumber = 0,
  };

  enum ProtoFieldType {
    kProtoNumber = 0,
  };

  struct Field {
    size_t ftrace_offset;
    size_t ftrace_size;
    FtraceFieldType ftrace_type;
    size_t proto_field_id;
    ProtoFieldType proto_field_type;
  };

  struct Event {
    std::string name;
    std::string group;
    std::vector<Field> fields;
    size_t ftrace_event_id;
    size_t proto_field_id;
  };

  static std::unique_ptr<ProtoTranslationTable> Create(
      const FtraceProcfs* ftrace_procfs);
  ~ProtoTranslationTable();

  ProtoTranslationTable(const std::vector<Event>& events,
                        std::vector<Field> common_fields);

  size_t largest_id() const { return largest_id_; }

  const std::vector<Field>& common_fields() const { return common_fields_; }

  const Event* GetEventByName(const std::string& name) const {
    if (!name_to_event_.count(name))
      return nullptr;
    return name_to_event_.at(name);
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

 private:
  ProtoTranslationTable(const ProtoTranslationTable&) = delete;
  ProtoTranslationTable& operator=(const ProtoTranslationTable&) = delete;

  const std::vector<Event> events_;
  size_t largest_id_;
  std::map<std::string, const Event*> name_to_event_;
  std::vector<Field> common_fields_;
};

}  // namespace perfetto

#endif  // FTRACE_PROTO_TRANSLATION_TABLE_H_
