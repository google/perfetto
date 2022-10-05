/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/traced/probes/ftrace/ftrace_print_filter.h"

#include <string.h>

#include "protos/perfetto/config/ftrace/ftrace_config.gen.h"
#include "src/traced/probes/ftrace/event_info_constants.h"

namespace perfetto {
namespace {
using ::perfetto::protos::gen::FtraceConfig;

bool Matches(const std::string& prefix, const char* start, size_t size) {
  if (prefix.size() > size) {
    return false;
  }
  return strncmp(prefix.c_str(), start, prefix.size()) == 0;
}

}  // namespace

FtracePrintFilter::FtracePrintFilter(const FtraceConfig::PrintFilter& conf) {
  rules_.reserve(conf.rules().size());
  for (const FtraceConfig::PrintFilter::Rule& conf_rule : conf.rules()) {
    Rule rule;
    rule.allow = conf_rule.allow();
    rule.prefix = conf_rule.prefix();
    rules_.push_back(std::move(rule));
  }
}

bool FtracePrintFilter::IsAllowed(const char* start, size_t size) const {
  for (const Rule& rule : rules_) {
    if (Matches(rule.prefix, start, size)) {
      return rule.allow;
    }
  }
  return true;
}

// static
base::Optional<FtracePrintFilterConfig> FtracePrintFilterConfig::Create(
    const protos::gen::FtraceConfig_PrintFilter& config,
    ProtoTranslationTable* table) {
  const Event* print_event = table->GetEvent(GroupAndName("ftrace", "print"));
  if (!print_event) {
    return base::nullopt;
  }
  const Field* buf_field = nullptr;
  for (const Field& field : print_event->fields) {
    if (strcmp(field.ftrace_name, "buf") == 0) {
      buf_field = &field;
      break;
    }
  }
  if (!buf_field) {
    return base::nullopt;
  }

  if (buf_field->strategy != kCStringToString) {
    return base::nullopt;
  }
  FtracePrintFilterConfig ret{FtracePrintFilter{config}};
  ret.event_id_ = print_event->ftrace_event_id;
  ret.event_size_ = print_event->size;
  ret.buf_field_offset_ = buf_field->ftrace_offset;
  return std::move(ret);
}

FtracePrintFilterConfig::FtracePrintFilterConfig(FtracePrintFilter filter)
    : filter_(filter) {}

bool FtracePrintFilterConfig::IsEventInteresting(const uint8_t* start,
                                                 const uint8_t* end) const {
  PERFETTO_DCHECK(start < end);
  const size_t length = static_cast<size_t>(end - start);

  // If the end of the buffer is before the end of the event, give up.
  if (event_size_ >= length) {
    PERFETTO_DFATAL("Buffer overflowed.");
    return true;
  }

  const uint8_t* field_start = start + buf_field_offset_;
  return filter_.IsAllowed(reinterpret_cast<const char*>(field_start),
                           static_cast<size_t>(end - field_start));
}

}  // namespace perfetto
