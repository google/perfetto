/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traced/probes/ftrace/ftrace_config.h"

#include "perfetto/base/logging.h"

namespace perfetto {
namespace {

bool IsGoodPunctuation(char c) {
  return c == '_' || c == '.';
}

bool IsValid(const std::string& str) {
  for (size_t i = 0; i < str.size(); i++) {
    if (!isalnum(str[i]) && !IsGoodPunctuation(str[i]))
      return false;
  }
  return true;
}

}  // namespace

std::set<std::string> FtraceEventsAsSet(const FtraceConfig& config) {
  std::set<std::string> events;
  for (const std::string& event : config.ftrace_events())
    events.insert(event);
  return events;
}

FtraceConfig CreateFtraceConfig(std::set<std::string> names) {
  FtraceConfig config;
  for (const std::string& name : names)
    *config.add_ftrace_events() = name;
  return config;
}

bool RequiresAtrace(const FtraceConfig& config) {
  return !config.atrace_categories().empty() || !config.atrace_apps().empty();
}

bool ValidConfig(const FtraceConfig& config) {
  for (const std::string& event_name : config.ftrace_events()) {
    if (!IsValid(event_name)) {
      PERFETTO_ELOG("Bad event name '%s'", event_name.c_str());
      return false;
    }
  }
  for (const std::string& category : config.atrace_categories()) {
    if (!IsValid(category)) {
      PERFETTO_ELOG("Bad category name '%s'", category.c_str());
      return false;
    }
  }
  for (const std::string& app : config.atrace_apps()) {
    if (!IsValid(app)) {
      PERFETTO_ELOG("Bad app '%s'", app.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace perfetto
