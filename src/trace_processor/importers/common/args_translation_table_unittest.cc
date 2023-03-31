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

#include <optional>

#include "src/trace_processor/importers/common/deobfuscation_mapping_table.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(ArgsTranslationTable, EmptyTableByDefault) {
  TraceStorage storage;
  ArgsTranslationTable table(&storage);
  EXPECT_EQ(table.TranslateChromeHistogramHashForTesting(1), std::nullopt);
  EXPECT_EQ(table.TranslateChromeUserEventHashForTesting(1), std::nullopt);
}

TEST(ArgsTranslationTable, TranslatesHistogramHashes) {
  TraceStorage storage;
  ArgsTranslationTable table(&storage);
  table.AddChromeHistogramTranslationRule(1, "hash1");
  table.AddChromeHistogramTranslationRule(10, "hash2");
  EXPECT_EQ(table.TranslateChromeHistogramHashForTesting(1),
            std::optional<base::StringView>("hash1"));
  EXPECT_EQ(table.TranslateChromeHistogramHashForTesting(10),
            std::optional<base::StringView>("hash2"));
  EXPECT_EQ(table.TranslateChromeHistogramHashForTesting(2), std::nullopt);
}

TEST(ArgsTranslationTable, TranslatesUserEventHashes) {
  TraceStorage storage;
  ArgsTranslationTable table(&storage);
  table.AddChromeUserEventTranslationRule(1, "action1");
  table.AddChromeUserEventTranslationRule(10, "action2");
  EXPECT_EQ(table.TranslateChromeUserEventHashForTesting(1),
            std::optional<base::StringView>("action1"));
  EXPECT_EQ(table.TranslateChromeUserEventHashForTesting(10),
            std::optional<base::StringView>("action2"));
  EXPECT_EQ(table.TranslateChromeUserEventHashForTesting(2), std::nullopt);
}

TEST(ArgsTranslationTable, TranslatesPerformanceMarkSiteHashes) {
  TraceStorage storage;
  ArgsTranslationTable table(&storage);
  table.AddChromePerformanceMarkSiteTranslationRule(1, "hash1");
  table.AddChromePerformanceMarkSiteTranslationRule(10, "hash2");
  EXPECT_EQ(table.TranslateChromePerformanceMarkSiteHashForTesting(1),
            std::optional<base::StringView>("hash1"));
  EXPECT_EQ(table.TranslateChromePerformanceMarkSiteHashForTesting(10),
            std::optional<base::StringView>("hash2"));
  EXPECT_EQ(table.TranslateChromePerformanceMarkSiteHashForTesting(2),
            std::nullopt);
}

TEST(ArgsTranslationTable, TranslatesPerformanceMarkMarkHashes) {
  TraceStorage storage;
  ArgsTranslationTable table(&storage);
  table.AddChromePerformanceMarkMarkTranslationRule(1, "hash1");
  table.AddChromePerformanceMarkMarkTranslationRule(10, "hash2");
  EXPECT_EQ(table.TranslateChromePerformanceMarkMarkHashForTesting(1),
            std::optional<base::StringView>("hash1"));
  EXPECT_EQ(table.TranslateChromePerformanceMarkMarkHashForTesting(10),
            std::optional<base::StringView>("hash2"));
  EXPECT_EQ(table.TranslateChromePerformanceMarkMarkHashForTesting(2),
            std::nullopt);
}

TEST(ArgsTranslationTable, TranslateClassName) {
  TraceStorage storage;
  StringId xyz_id = storage.InternString("xyz");
  StringId abc_id = storage.InternString("abc");
  StringId class_x_id = storage.InternString("class_X");
  DeobfuscationMappingTable deobfuscation_mapping;
  deobfuscation_mapping.AddClassTranslation(
      DeobfuscationMappingTable::PackageId{"app", 123}, xyz_id, class_x_id,
      base::FlatHashMap<StringId, StringId>{});
  ArgsTranslationTable table(&storage);
  table.AddDeobfuscationMappingTable(std::move(deobfuscation_mapping));

  EXPECT_EQ(table.TranslateClassNameForTesting(xyz_id),
            std::optional<StringId>(class_x_id));
  EXPECT_EQ(table.TranslateClassNameForTesting(abc_id), std::nullopt);
}

TEST(ArgsTranslationTable, NeedsTranslation) {
  TraceStorage storage;
  ArgsTranslationTable table(&storage);

  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("unused_flat_key"),
      storage.InternString("chrome_histogram_sample.name_hash"),
      Variadic::Type::kUint));
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("unused_flat_key"),
      storage.InternString("chrome_user_event.action_hash"),
      Variadic::Type::kUint));
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("unused_flat_key"),
      storage.InternString("chrome_hashed_performance_mark.site_hash"),
      Variadic::Type::kUint));
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("unused_flat_key"),
      storage.InternString("chrome_hashed_performance_mark.mark_hash"),
      Variadic::Type::kUint));

  // A real life case, where flat_key == key.
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("chrome_histogram_sample.name_hash"),
      storage.InternString("chrome_histogram_sample.name_hash"),
      Variadic::Type::kUint));
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("chrome_user_event.action_hash"),
      storage.InternString("chrome_user_event.action_hash"),
      Variadic::Type::kUint));
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("chrome_hashed_performance_mark.site_hash"),
      storage.InternString("chrome_hashed_performance_mark.site_hash"),
      Variadic::Type::kUint));
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("chrome_hashed_performance_mark.mark_hash"),
      storage.InternString("chrome_hashed_performance_mark.mark_hash"),
      Variadic::Type::kUint));

  // The key needs translation, but the arg type is wrong (not uint).
  EXPECT_FALSE(table.NeedsTranslation(
      storage.InternString("unused_flat_key"),
      storage.InternString("chrome_histogram_sample.name_hash"),
      Variadic::Type::kInt));
  // The key does not require translation.
  EXPECT_FALSE(table.NeedsTranslation(
      storage.InternString("unused_flat_key"),
      storage.InternString("chrome_histogram_sample.name"),
      Variadic::Type::kUint));

  // The key needs translation by flat_key.
  EXPECT_TRUE(table.NeedsTranslation(
      storage.InternString("android_view_dump.activity.view.class_name"),
      storage.InternString("android_view_dump.activity[0].view[0].class_name"),
      Variadic::Type::kString));
  // The key does not require translation because flat_key and key are swapped.
  EXPECT_FALSE(table.NeedsTranslation(
      storage.InternString("android_view_dump.activity[0].view[0].class_name"),
      storage.InternString("android_view_dump.activity.view.class_name"),
      Variadic::Type::kString));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
