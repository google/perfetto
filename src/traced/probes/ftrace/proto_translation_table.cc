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

#include "src/traced/probes/ftrace/proto_translation_table.h"

#include <regex.h>

#include <algorithm>

#include "perfetto/base/string_utils.h"
#include "src/traced/probes/ftrace/event_info.h"
#include "src/traced/probes/ftrace/ftrace_procfs.h"

#include "perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto {

namespace {

ProtoTranslationTable::FtracePageHeaderSpec MakeFtracePageHeaderSpec(
    const std::vector<FtraceEvent::Field>& fields) {
  ProtoTranslationTable::FtracePageHeaderSpec spec;
  for (const FtraceEvent::Field& field : fields) {
    std::string name = GetNameFromTypeAndName(field.type_and_name);
    if (name == "timestamp")
      spec.timestamp = field;
    else if (name == "commit")
      spec.size = field;
    else if (name == "overwrite")
      spec.overwrite = field;
    else if (name != "data")
      PERFETTO_DCHECK(false);
  }
  return spec;
}

const std::vector<Event> BuildEventsVector(const std::vector<Event>& events) {
  size_t largest_id = 0;
  for (const Event& event : events) {
    if (event.ftrace_event_id > largest_id)
      largest_id = event.ftrace_event_id;
  }
  std::vector<Event> events_by_id;
  events_by_id.resize(largest_id + 1);
  for (const Event& event : events) {
    events_by_id[event.ftrace_event_id] = event;
  }
  events_by_id.shrink_to_fit();
  return events_by_id;
}

// Merge the information from |ftrace_field| into |field| (mutating it).
// We should set the following fields: offset, size, ftrace field type and
// translation strategy.
bool MergeFieldInfo(const FtraceEvent::Field& ftrace_field,
                    Field* field,
                    const char* event_name_for_debug) {
  PERFETTO_DCHECK(field->ftrace_name);
  PERFETTO_DCHECK(field->proto_field_id);
  PERFETTO_DCHECK(field->proto_field_type);
  PERFETTO_DCHECK(!field->ftrace_offset);
  PERFETTO_DCHECK(!field->ftrace_size);
  PERFETTO_DCHECK(!field->ftrace_type);

  if (!InferFtraceType(ftrace_field.type_and_name, ftrace_field.size,
                       ftrace_field.is_signed, &field->ftrace_type)) {
    PERFETTO_DLOG(
        "Failed to infer ftrace field type for \"%s.%s\" (type:\"%s\" size:%d "
        "signed:%d)",
        event_name_for_debug, field->ftrace_name,
        ftrace_field.type_and_name.c_str(), ftrace_field.size,
        ftrace_field.is_signed);
    PERFETTO_DCHECK(false);
    return false;
  }

  field->ftrace_offset = ftrace_field.offset;
  field->ftrace_size = ftrace_field.size;

  if (!SetTranslationStrategy(field->ftrace_type, field->proto_field_type,
                              &field->strategy)) {
    PERFETTO_DLOG(
        "Failed to find translation stratagy for ftrace field \"%s.%s\" (%s -> "
        "%s)",
        event_name_for_debug, field->ftrace_name, ToString(field->ftrace_type),
        ToString(field->proto_field_type));
    // TODO(hjd): Uncomment DCHECK when proto generation is fixed.
    // PERFETTO_DCHECK(false);
    return false;
  }

  return true;
}

// For each field in |fields| find the matching field from |ftrace_fields| (by
// comparing ftrace_name) and copy the information from the FtraceEvent::Field
// into the Field (mutating it). If there is no matching field in
// |ftrace_fields| remove the Field from |fields|. Return the maximum observed
// 'field end' (offset + size).
uint16_t MergeFields(const std::vector<FtraceEvent::Field>& ftrace_fields,
                     std::vector<Field>* fields,
                     const char* event_name_for_debug) {
  uint16_t fields_end = 0;

  // Loop over each Field in |fields| modifiying it with information from the
  // matching |ftrace_fields| field or removing it.
  auto field = fields->begin();
  while (field != fields->end()) {
    bool success = false;
    for (const FtraceEvent::Field& ftrace_field : ftrace_fields) {
      if (GetNameFromTypeAndName(ftrace_field.type_and_name) !=
          field->ftrace_name)
        continue;

      success = MergeFieldInfo(ftrace_field, &*field, event_name_for_debug);

      uint16_t field_end = field->ftrace_offset + field->ftrace_size;
      fields_end = std::max<uint16_t>(fields_end, field_end);

      break;
    }
    if (success) {
      ++field;
    } else {
      field = fields->erase(field);
    }
  }
  return fields_end;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string RegexError(int errcode, const regex_t* preg) {
  char buf[64];
  regerror(errcode, preg, buf, sizeof(buf));
  return {buf, sizeof(buf)};
}

bool Match(const char* string, const char* pattern) {
  regex_t re;
  int ret = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
  if (ret != 0) {
    PERFETTO_FATAL("regcomp: %s", RegexError(ret, &re).c_str());
  }
  ret = regexec(&re, string, 0, nullptr, 0);
  regfree(&re);
  return ret != REG_NOMATCH;
}

}  // namespace

// This is similar but different from InferProtoType (see format_parser.cc).
// TODO(hjd): Fold FtraceEvent(::Field) into Event.
bool InferFtraceType(const std::string& type_and_name,
                     size_t size,
                     bool is_signed,
                     FtraceFieldType* out) {
  // Fixed length strings: e.g. "char foo[16]" we don't care about the number
  // since we get the size as it's own field. Somewhat awkwardly these fields
  // are both fixed size and null terminated meaning that we can't just drop
  // them directly into the protobuf (since if the string is shorter than 15
  // characters we want only the bit up to the null terminator).
  if (Match(type_and_name.c_str(), R"(char [a-zA-Z_]+\[[0-9]+\])")) {
    *out = kFtraceFixedCString;
    return true;
  }

  // String pointers: "__data_loc char[] foo" (as in
  // 'cpufreq_interactive_boost').
  // TODO(fmayer): Handle u32[], u8[], __u8[] as well.
  if (Contains(type_and_name, "__data_loc char[] ")) {
    if (size != 4) {
      PERFETTO_ELOG("__data_loc with incorrect size: %s (%zd)",
                    type_and_name.c_str(), size);
      return false;
    }
    *out = kFtraceDataLoc;
    return true;
  }

  if (Contains(type_and_name, "char[] ")) {
    *out = kFtraceStringPtr;
    return true;
  }
  if (Contains(type_and_name, "char * ")) {
    *out = kFtraceStringPtr;
    return true;
  }

  // Variable length strings: "char foo" + size: 0 (as in 'print').
  if (base::StartsWith(type_and_name, "char ") && size == 0) {
    *out = kFtraceCString;
    return true;
  }

  if (base::StartsWith(type_and_name, "bool ")) {
    *out = kFtraceBool;
    return true;
  }

  if (base::StartsWith(type_and_name, "ino_t ") ||
      base::StartsWith(type_and_name, "i_ino ")) {
    if (size == 4) {
      *out = kFtraceInode32;
      return true;
    } else if (size == 8) {
      *out = kFtraceInode64;
      return true;
    }
  }

  if (base::StartsWith(type_and_name, "dev_t ")) {
    if (size == 4) {
      *out = kFtraceDevId32;
      return true;
    } else if (size == 8) {
      *out = kFtraceDevId64;
      return true;
    }
  }

  // Pids (as in 'sched_switch').
  if (base::StartsWith(type_and_name, "pid_t ") && size == 4) {
    *out = kFtracePid32;
    return true;
  }

  if (Contains(type_and_name, "common_pid") && size == 4) {
    *out = kFtraceCommonPid32;
    return true;
  }

  // Ints of various sizes:
  if (size == 1 && is_signed) {
    *out = kFtraceInt8;
    return true;
  } else if (size == 1 && !is_signed) {
    *out = kFtraceUint8;
    return true;
  } else if (size == 2 && is_signed) {
    *out = kFtraceInt16;
    return true;
  } else if (size == 2 && !is_signed) {
    *out = kFtraceUint16;
    return true;
  } else if (size == 4 && is_signed) {
    *out = kFtraceInt32;
    return true;
  } else if (size == 4 && !is_signed) {
    *out = kFtraceUint32;
    return true;
  } else if (size == 8 && is_signed) {
    *out = kFtraceInt64;
    return true;
  } else if (size == 8 && !is_signed) {
    *out = kFtraceUint64;
    return true;
  }

  PERFETTO_DLOG("Could not infer ftrace type for '%s'", type_and_name.c_str());
  return false;
}

// static
ProtoTranslationTable::FtracePageHeaderSpec
ProtoTranslationTable::DefaultPageHeaderSpecForTesting() {
  std::string page_header =
      R"(	field: u64 timestamp;	offset:0;	size:8;	signed:0;
	field: local_t commit;	offset:8;	size:8;	signed:1;
	field: int overwrite;	offset:8;	size:1;	signed:1;
	field: char data;	offset:16;	size:4080;	signed:0;)";
  std::vector<FtraceEvent::Field> page_header_fields;
  PERFETTO_CHECK(ParseFtraceEventBody(std::move(page_header), nullptr,
                                      &page_header_fields));
  return MakeFtracePageHeaderSpec(page_header_fields);
}

// static
std::unique_ptr<ProtoTranslationTable> ProtoTranslationTable::Create(
    const FtraceProcfs* ftrace_procfs,
    std::vector<Event> events,
    std::vector<Field> common_fields) {
  bool common_fields_processed = false;
  uint16_t common_fields_end = 0;

  std::vector<FtraceEvent::Field> page_header_fields;
  std::string page_header = ftrace_procfs->ReadPageHeaderFormat();
  PERFETTO_CHECK(!page_header.empty());
  PERFETTO_CHECK(ParseFtraceEventBody(std::move(page_header), nullptr,
                                      &page_header_fields));

  for (Event& event : events) {
    PERFETTO_DCHECK(event.name);
    PERFETTO_DCHECK(event.group);
    PERFETTO_DCHECK(event.proto_field_id);
    PERFETTO_DCHECK(!event.ftrace_event_id);

    std::string contents =
        ftrace_procfs->ReadEventFormat(event.group, event.name);
    FtraceEvent ftrace_event;
    if (contents.empty() || !ParseFtraceEvent(contents, &ftrace_event)) {
      continue;
    }

    event.ftrace_event_id = ftrace_event.id;

    if (!common_fields_processed) {
      common_fields_end =
          MergeFields(ftrace_event.common_fields, &common_fields, event.name);
      common_fields_processed = true;
    }

    uint16_t fields_end =
        MergeFields(ftrace_event.fields, &event.fields, event.name);

    event.size = std::max<uint16_t>(fields_end, common_fields_end);
  }

  events.erase(std::remove_if(events.begin(), events.end(),
                              [](const Event& event) {
                                return event.proto_field_id == 0 ||
                                       event.ftrace_event_id == 0;
                              }),
               events.end());

  auto table = std::unique_ptr<ProtoTranslationTable>(
      new ProtoTranslationTable(events, std::move(common_fields),
                                MakeFtracePageHeaderSpec(page_header_fields)));
  return table;
}

ProtoTranslationTable::ProtoTranslationTable(
    const std::vector<Event>& events,
    std::vector<Field> common_fields,
    FtracePageHeaderSpec ftrace_page_header_spec)
    : events_(BuildEventsVector(events)),
      largest_id_(events_.size() - 1),
      common_fields_(std::move(common_fields)),
      ftrace_page_header_spec_(ftrace_page_header_spec) {
  for (const Event& event : events) {
    name_to_event_[event.name] = &events_.at(event.ftrace_event_id);
    group_to_events_[event.group].push_back(&events_.at(event.ftrace_event_id));
  }
}

ProtoTranslationTable::~ProtoTranslationTable() = default;

}  // namespace perfetto
