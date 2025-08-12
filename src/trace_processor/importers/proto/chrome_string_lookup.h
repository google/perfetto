/*
 * Copyright (C) 2021 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_CHROME_STRING_LOOKUP_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_CHROME_STRING_LOOKUP_H_

#include <array>

#include "protos/third_party/chromium/chrome_enums.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class ChromeStringLookup {
 public:
  // Min and max known values for process and thread types.
  constexpr static int32_t kProcessTypeMin =
      ::perfetto::protos::chrome_enums::pbzero::ProcessType_MIN;
  constexpr static int32_t kProcessTypeMax =
      ::perfetto::protos::chrome_enums::pbzero::ProcessType_MAX;
  constexpr static int32_t kThreadTypeMin =
      ::perfetto::protos::chrome_enums::pbzero::ThreadType_MIN;
  constexpr static int32_t kThreadTypeMax =
      ::perfetto::protos::chrome_enums::pbzero::ThreadType_MAX;

  explicit ChromeStringLookup(TraceStorage* storage,
                              bool ignore_predefined_names_for_testing = false);

  StringId GetProcessName(int32_t process_type) const;
  StringId GetThreadName(int32_t thread_type) const;

 private:
  std::array<StringId, kProcessTypeMax - kProcessTypeMin + 1>
      chrome_process_name_ids_;
  std::array<StringId, kThreadTypeMax - kThreadTypeMin + 1>
      chrome_thread_name_ids_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_CHROME_STRING_LOOKUP_H_
