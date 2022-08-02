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

constexpr char ArgsTranslationTable::kMojoMethodMappingIdKey[];
constexpr char ArgsTranslationTable::kMojoMethodRelPcKey[];
constexpr char ArgsTranslationTable::kMojoMethodNameKey[];

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
          storage->InternString(kChromePerformanceMarkMarkKey)),
      interned_mojo_method_mapping_id_(
          storage->InternString(kMojoMethodMappingIdKey)),
      interned_mojo_method_rel_pc_(storage->InternString(kMojoMethodRelPcKey)),
      interned_mojo_method_name_(storage->InternString(kMojoMethodNameKey)) {}

bool ArgsTranslationTable::NeedsTranslation(StringId key_id,
                                            Variadic::Type type) const {
  return KeyIdAndTypeToEnum(key_id, type).has_value();
}

void ArgsTranslationTable::TranslateArgs(
    const ArgsTracker::CompactArgSet& arg_set,
    ArgsTracker::BoundInserter& inserter) const {
  base::Optional<uint64_t> mapping_id;
  base::Optional<uint64_t> rel_pc;

  for (const auto& arg : arg_set) {
    const auto key_type = KeyIdAndTypeToEnum(arg.key, arg.value.type);
    if (!key_type.has_value()) {
      inserter.AddArg(arg.key, arg.value, arg.update_policy);
      continue;
    }

    switch (*key_type) {
      case KeyType::kChromeHistogramHash: {
        inserter.AddArg(interned_chrome_histogram_hash_key_, arg.value);
        const base::Optional<base::StringView> translated_value =
            TranslateChromeHistogramHash(arg.value.uint_value);
        if (translated_value) {
          inserter.AddArg(
              interned_chrome_histogram_name_key_,
              Variadic::String(storage_->InternString(*translated_value)));
        }
        break;
      }
      case KeyType::kChromeUserEventHash: {
        inserter.AddArg(interned_chrome_user_event_hash_key_, arg.value);
        const base::Optional<base::StringView> translated_value =
            TranslateChromeUserEventHash(arg.value.uint_value);
        if (translated_value) {
          inserter.AddArg(
              interned_chrome_user_event_action_key_,
              Variadic::String(storage_->InternString(*translated_value)));
        }
        break;
      }
      case KeyType::kChromePerformanceMarkMarkHash: {
        inserter.AddArg(interned_chrome_performance_mark_mark_hash_key_,
                        arg.value);
        const base::Optional<base::StringView> translated_value =
            TranslateChromePerformanceMarkMarkHash(arg.value.uint_value);
        if (translated_value) {
          inserter.AddArg(
              interned_chrome_performance_mark_mark_key_,
              Variadic::String(storage_->InternString(*translated_value)));
        }
        break;
      }
      case KeyType::kChromePerformanceMarkSiteHash: {
        inserter.AddArg(interned_chrome_performance_mark_site_hash_key_,
                        arg.value);
        const base::Optional<base::StringView> translated_value =
            TranslateChromePerformanceMarkSiteHash(arg.value.uint_value);
        if (translated_value) {
          inserter.AddArg(
              interned_chrome_performance_mark_site_key_,
              Variadic::String(storage_->InternString(*translated_value)));
        }
        break;
      }
      case KeyType::kMojoMethodMappingId: {
        mapping_id = arg.value.uint_value;
        break;
      }
      case KeyType::kMojoMethodRelPc: {
        rel_pc = arg.value.uint_value;
        break;
      }
    }
  }
  EmitMojoMethodLocation(mapping_id, rel_pc, inserter);
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
  if (key_id == interned_mojo_method_mapping_id_) {
    return KeyType::kMojoMethodMappingId;
  }
  if (key_id == interned_mojo_method_rel_pc_) {
    return KeyType::kMojoMethodRelPc;
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

base::Optional<ArgsTranslationTable::SourceLocation>
ArgsTranslationTable::TranslateNativeSymbol(MappingId mapping_id,
                                            uint64_t rel_pc) const {
  auto loc =
      native_symbol_to_location_.Find(std::make_pair(mapping_id, rel_pc));
  if (!loc) {
    return base::nullopt;
  }
  return *loc;
}

void ArgsTranslationTable::EmitMojoMethodLocation(
    base::Optional<uint64_t> mapping_id,
    base::Optional<uint64_t> rel_pc,
    ArgsTracker::BoundInserter& inserter) const {
  if (!mapping_id || !rel_pc) {
    return;
  }
  const MappingId row_id(static_cast<uint32_t>(*mapping_id));
  const auto loc = TranslateNativeSymbol(row_id, *rel_pc);
  if (loc) {
    inserter.AddArg(interned_mojo_method_name_,
                    Variadic::String(storage_->InternString(
                        base::StringView(loc->function_name))));
  } else {
    // Could not find the corresponding source location. Let's emit raw arg
    // values so that the data doesn't silently go missing.
    inserter.AddArg(interned_mojo_method_mapping_id_,
                    Variadic::UnsignedInteger(*mapping_id));
    inserter.AddArg(interned_mojo_method_rel_pc_,
                    Variadic::UnsignedInteger(*rel_pc));
  }
}

}  // namespace trace_processor
}  // namespace perfetto
