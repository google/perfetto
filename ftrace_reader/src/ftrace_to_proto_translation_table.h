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

#ifndef FTRACE_TO_PROTO_TRANSLATION_TABLE_H_
#define FTRACE_TO_PROTO_TRANSLATION_TABLE_H_

#include <stdint.h>

#include "base/scoped_file.h"
#include "ftrace_event_bundle.pbzero.h"

namespace perfetto {

class FtraceToProtoTranslationTable {
 public:
  FtraceToProtoTranslationTable();
  ~FtraceToProtoTranslationTable();

 private:
  FtraceToProtoTranslationTable(const FtraceToProtoTranslationTable&) = delete;
  FtraceToProtoTranslationTable& operator=(
      const FtraceToProtoTranslationTable&) = delete;
};

}  // namespace perfetto

#endif  // FTRACE_TO_PROTO_TRANSLATION_TABLE_H_
