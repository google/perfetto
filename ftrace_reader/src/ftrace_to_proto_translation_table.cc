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

#include "ftrace_to_proto_translation_table.h"

namespace perfetto {

// static
std::unique_ptr<FtraceToProtoTranslationTable>
FtraceToProtoTranslationTable::Create(std::string path_to_event_dir) {
  std::map<size_t, Event> events;
  std::vector<Field> common_fields;
  auto table = std::unique_ptr<FtraceToProtoTranslationTable>(
      new FtraceToProtoTranslationTable(std::move(events),
                                        std::move(common_fields)));
  return table;
}

FtraceToProtoTranslationTable::FtraceToProtoTranslationTable(
    std::map<size_t, Event> events,
    std::vector<Field> common_fields)
    : events_(std::move(events)), common_fields_(std::move(common_fields)) {}

FtraceToProtoTranslationTable::~FtraceToProtoTranslationTable() = default;

}  // namespace perfetto
