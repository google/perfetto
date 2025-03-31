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

#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "src/traced/probes/ftrace/compact_sched.h"
#include "src/traced/probes/ftrace/format_parser/format_parser.h"
#include "src/traced/probes/ftrace/printk_formats_parser.h"

namespace perfetto {

class FtraceProcfs;

namespace protos {
namespace pbzero {
class FtraceEventBundle;
}  // namespace pbzero
}  // namespace protos

// Used when reading the config to store the group and name info for the
// ftrace event.
class GroupAndName {
 public:
  GroupAndName(std::string_view group, std::string_view name)
      : group_(group), name_(name) {}

  bool operator==(const GroupAndName& other) const {
    return std::tie(group_, name_) == std::tie(other.group(), other.name());
  }

  bool operator<(const GroupAndName& other) const {
    return std::tie(group_, name_) < std::tie(other.group(), other.name());
  }

  const std::string& name() const { return name_; }
  const std::string& group() const { return group_; }

  std::string ToString() const { return group_ + "/" + name_; }

 private:
  std::string group_;
  std::string name_;
};

inline void PrintTo(const GroupAndName& event, ::std::ostream* os) {
  *os << "GroupAndName(" << event.group() << ", " << event.name() << ")";
}

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
  virtual ~ProtoTranslationTable();

  ProtoTranslationTable(const ProtoTranslationTable&) = delete;
  ProtoTranslationTable& operator=(const ProtoTranslationTable&) = delete;

  ProtoTranslationTable(const FtraceProcfs* ftrace_procfs,
                        const std::vector<Event>& events,
                        std::vector<Field> common_fields,
                        FtracePageHeaderSpec ftrace_page_header_spec,
                        CompactSchedEventFormat compact_sched_format,
                        PrintkMap printk_formats);

  size_t largest_id() const { return largest_id_; }

  const std::vector<Field>& common_fields() const { return common_fields_; }

  const Field* common_pid() const {
    // corner case: pKVM hypervisor pseudo-tracefs lacks common_pid
    if (!common_pid_.has_value())
      return nullptr;
    return &common_pid_.value();
  }

  // Virtual for testing.
  virtual const Event* GetEvent(const GroupAndName& group_and_name) const {
    if (!group_and_name_to_event_.count(group_and_name))
      return nullptr;
    return group_and_name_to_event_.at(group_and_name);
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
    const Event* evt = &events_[id];
    if (!evt->ftrace_event_id)
      return nullptr;
    return evt;
  }

  size_t EventToFtraceId(const GroupAndName& group_and_name) const {
    if (!group_and_name_to_event_.count(group_and_name))
      return 0;
    return group_and_name_to_event_.at(group_and_name)->ftrace_event_id;
  }

  const std::deque<Event>& events() { return events_; }
  const FtracePageHeaderSpec& ftrace_page_header_spec() const {
    return ftrace_page_header_spec_;
  }

  // Returns the size in bytes of the "size" field in the ftrace header. This
  // usually matches sizeof(void*) in the kernel (which can be != sizeof(void*)
  // of user space on 32bit-user + 64-bit-kernel configurations).
  uint16_t page_header_size_len() const {
    // TODO(fmayer): Do kernel deepdive to double check this.
    return ftrace_page_header_spec_.size.size;
  }

  // Retrieves the ftrace event from the proto translation
  // table. If it does not exist, reads the format file and creates a
  // new event with the proto id set to generic. Virtual for testing.
  virtual const Event* GetOrCreateEvent(const GroupAndName&);

  // Retrieves the ftrace event, that's going to be translated to a kprobe, from
  // the proto translation table. If the event is already known and used for
  // something other than a kprobe, returns nullptr.
  virtual const Event* GetOrCreateKprobeEvent(const GroupAndName&);

  // Removes the ftrace event from the proto translation table.
  virtual void RemoveEvent(const GroupAndName&);

  // This is for backwards compatibility. If a group is not specified in the
  // config then the first event with that name will be returned.
  const Event* GetEventByName(const std::string& name) const {
    if (!name_to_events_.count(name))
      return nullptr;
    return name_to_events_.at(name)[0];
  }

  const CompactSchedEventFormat& compact_sched_format() const {
    return compact_sched_format_;
  }

  base::StringView LookupTraceString(uint64_t address) const {
    return printk_formats_.at(address);
  }

 private:
  // Store strings so they can be read when writing the trace output.
  const char* InternString(const std::string& str);

  // The event must not already exist.
  const Event* CreateEventWithProtoId(const GroupAndName& group_and_name,
                                      uint32_t proto_field_id);

  uint16_t CreateGenericEventField(const FtraceEvent::Field& ftrace_field,
                                   Event& event);

  const FtraceProcfs* ftrace_procfs_;
  std::deque<Event> events_;
  size_t largest_id_;
  std::map<GroupAndName, const Event*> group_and_name_to_event_;
  std::map<std::string, std::vector<const Event*>> name_to_events_;
  std::map<std::string, std::vector<const Event*>> group_to_events_;
  std::vector<Field> common_fields_;
  std::optional<Field> common_pid_;  // copy of entry in common_fields_
  FtracePageHeaderSpec ftrace_page_header_spec_{};
  std::set<std::string> interned_strings_;
  CompactSchedEventFormat compact_sched_format_;
  PrintkMap printk_formats_;
};

// Class for efficient 'is event with id x enabled?' checks.
// Mirrors the data in a FtraceConfig but in a format better suited
// to be consumed by CpuReader.
class EventFilter {
 public:
  EventFilter() = default;
  ~EventFilter() = default;
  // move-only
  EventFilter(EventFilter&&) = default;
  EventFilter& operator=(EventFilter&&) = default;
  EventFilter(const EventFilter&) = delete;
  EventFilter& operator=(const EventFilter&) = delete;

  void AddEnabledEvent(size_t ftrace_event_id);
  void DisableEvent(size_t ftrace_event_id);
  bool IsEventEnabled(size_t ftrace_event_id) const;
  std::set<size_t> GetEnabledEvents() const;
  void EnableEventsFrom(const EventFilter&);

 private:
  std::vector<bool> enabled_ids_;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_PROTO_TRANSLATION_TABLE_H_
