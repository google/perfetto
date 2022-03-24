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

#include "src/trace_processor/importers/common/args_translation_table.h"

namespace perfetto {
namespace trace_processor {

constexpr char ArgsTranslationTable::kChromeHistogramHashKey[];
constexpr char ArgsTranslationTable::kChromeHistogramNameKey[];

ArgsTranslationTable::ArgsTranslationTable(TraceStorage* storage)
    : storage_(storage),
      interned_chrome_histogram_name_key_(
          storage->InternString(kChromeHistogramNameKey)) {}

bool ArgsTranslationTable::TranslateUnsignedIntegerArg(
    const Key& key,
    uint64_t value,
    ArgsTracker::BoundInserter& inserter) {
  if (key.key == kChromeHistogramHashKey) {
    const base::Optional<base::StringView> translated_value =
        TranslateChromeHistogramHash(value);
    if (translated_value) {
      inserter.AddArg(
          interned_chrome_histogram_name_key_,
          Variadic::String(storage_->InternString(*translated_value)));
    }
  }
  return false;
}

base::Optional<base::StringView>
ArgsTranslationTable::TranslateChromeHistogramHashForTesting(
    uint64_t hash) const {
  return TranslateChromeHistogramHash(hash);
}

base::Optional<base::StringView>
ArgsTranslationTable::TranslateChromeHistogramHash(uint64_t hash) const {
  auto* value = chrome_histogram_hash_to_name_.Find(hash);
  if (!value) {
    return base::nullopt;
  }
  return base::StringView(*value);
}

void ArgsTranslationTable::AddChromeHistogramTranslationRule(
    uint64_t hash,
    base::StringView name) {
  chrome_histogram_hash_to_name_.Insert(hash, name.ToStdString());
}

}  // namespace trace_processor
}  // namespace perfetto
