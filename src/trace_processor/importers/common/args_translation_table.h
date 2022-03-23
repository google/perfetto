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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARGS_TRANSLATION_TABLE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARGS_TRANSLATION_TABLE_H_

#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"

namespace perfetto {
namespace trace_processor {

// Tracks and stores args translation rules. It allows Trace Processor
// to map for example hashes to their names.
class ArgsTranslationTable {
 public:
  static constexpr char kChromeHistogramHashKey[] =
      "chrome_histogram_sample.name_hash";
  static constexpr char kChromeHistogramNameKey[] =
      "chrome_histogram_sample.name";

  base::Optional<base::StringView> TranslateChromeHistogramHash(
      uint64_t hash) const;

  void AddChromeHistogramTranslationRule(uint64_t hash, base::StringView name);

 private:
  base::FlatHashMap<uint64_t, std::string> chrome_histogram_hash_to_name_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARGS_TRANSLATION_TABLE_H_
