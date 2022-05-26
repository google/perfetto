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

constexpr char ArgsTranslationTable::kChromeUserEventHashKey[];
constexpr char ArgsTranslationTable::kChromeUserEventActionKey[];

constexpr char ArgsTranslationTable::kChromePerformanceMarkSiteHashKey[];
constexpr char ArgsTranslationTable::kChromePerformanceMarkSiteKey[];

constexpr char ArgsTranslationTable::kChromePerformanceMarkMarkHashKey[];
constexpr char ArgsTranslationTable::kChromePerformanceMarkMarkKey[];

ArgsTranslationTable::ArgsTranslationTable(TraceStorage* storage)
    : storage_(storage),
      interned_chrome_histogram_hash_key_(
          storage->InternString(kChromeHistogramHashKey)),
      interned_chrome_histogram_name_key_(
          storage->InternString(kChromeHistogramNameKey)),
      interned_chrome_user_event_hash_key_(
          storage->InternString(kChromeUserEventHashKey)),
      interned_chrome_user_event_action_key_(
          storage->InternString(kChromeUserEventActionKey)),
      interned_chrome_performance_mark_site_hash_key_(
          storage->InternString(kChromePerformanceMarkSiteHashKey)),
      interned_chrome_performance_mark_site_key_(
          storage->InternString(kChromePerformanceMarkSiteKey)),
      interned_chrome_performance_mark_mark_hash_key_(
          storage->InternString(kChromePerformanceMarkMarkHashKey)),
      interned_chrome_performance_mark_mark_key_(
          storage->InternString(kChromePerformanceMarkMarkKey)) {}

bool ArgsTranslationTable::NeedsTranslation(StringId key_id,
                                            Variadic::Type type) const {
  return KeyIdAndTypeToEnum(key_id, type).has_value();
}

bool ArgsTranslationTable::TranslateArg(StringId key_id,
                                        Variadic value,
                                        ArgsTracker::BoundInserter& inserter) {
  const auto key_type = KeyIdAndTypeToEnum(key_id, value.type);
  if (!key_type.has_value()) {
    return false;
  }
  switch (*key_type) {
    case KeyType::kChromeHistogramHash: {
      inserter.AddArg(interned_chrome_histogram_hash_key_, value);
      const base::Optional<base::StringView> translated_value =
          TranslateChromeHistogramHash(value.uint_value);
      if (translated_value) {
        inserter.AddArg(
            interned_chrome_histogram_name_key_,
            Variadic::String(storage_->InternString(*translated_value)));
      }
      return true;
    }
    case KeyType::kChromeUserEventHash: {
      inserter.AddArg(interned_chrome_user_event_hash_key_, value);
      const base::Optional<base::StringView> translated_value =
          TranslateChromeUserEventHash(value.uint_value);
      if (translated_value) {
        inserter.AddArg(
            interned_chrome_user_event_action_key_,
            Variadic::String(storage_->InternString(*translated_value)));
      }
      return true;
    }
    case KeyType::kChromePerformanceMarkMarkHash: {
      inserter.AddArg(interned_chrome_performance_mark_mark_hash_key_, value);
      const base::Optional<base::StringView> translated_value =
          TranslateChromePerformanceMarkMarkHash(value.uint_value);
      if (translated_value) {
        inserter.AddArg(
            interned_chrome_performance_mark_mark_key_,
            Variadic::String(storage_->InternString(*translated_value)));
      }
      return true;
    }
    case KeyType::kChromePerformanceMarkSiteHash: {
      inserter.AddArg(interned_chrome_performance_mark_site_hash_key_, value);
      const base::Optional<base::StringView> translated_value =
          TranslateChromePerformanceMarkSiteHash(value.uint_value);
      if (translated_value) {
        inserter.AddArg(
            interned_chrome_performance_mark_site_key_,
            Variadic::String(storage_->InternString(*translated_value)));
      }
      return true;
    }
  }
  return false;
}

base::Optional<ArgsTranslationTable::KeyType>
ArgsTranslationTable::KeyIdAndTypeToEnum(StringId key_id,
                                         Variadic::Type type) const {
  if (type != Variadic::Type::kUint) {
    return base::nullopt;
  }
  if (key_id == interned_chrome_histogram_hash_key_) {
    return KeyType::kChromeHistogramHash;
  }
  if (key_id == interned_chrome_user_event_hash_key_) {
    return KeyType::kChromeUserEventHash;
  }
  if (key_id == interned_chrome_performance_mark_mark_hash_key_) {
    return KeyType::kChromePerformanceMarkMarkHash;
  }
  if (key_id == interned_chrome_performance_mark_site_hash_key_) {
    return KeyType::kChromePerformanceMarkSiteHash;
  }
  return base::nullopt;
}

base::Optional<base::StringView>
ArgsTranslationTable::TranslateChromeHistogramHash(uint64_t hash) const {
  auto* value = chrome_histogram_hash_to_name_.Find(hash);
  if (!value) {
    return base::nullopt;
  }
  return base::StringView(*value);
}

base::Optional<base::StringView>
ArgsTranslationTable::TranslateChromeUserEventHash(uint64_t hash) const {
  auto* value = chrome_user_event_hash_to_action_.Find(hash);
  if (!value) {
    return base::nullopt;
  }
  return base::StringView(*value);
}

base::Optional<base::StringView>
ArgsTranslationTable::TranslateChromePerformanceMarkSiteHash(
    uint64_t hash) const {
  auto* value = chrome_performance_mark_site_hash_to_name_.Find(hash);
  if (!value) {
    return base::nullopt;
  }
  return base::StringView(*value);
}

base::Optional<base::StringView>
ArgsTranslationTable::TranslateChromePerformanceMarkMarkHash(
    uint64_t hash) const {
  auto* value = chrome_performance_mark_mark_hash_to_name_.Find(hash);
  if (!value) {
    return base::nullopt;
  }
  return base::StringView(*value);
}

}  // namespace trace_processor
}  // namespace perfetto
