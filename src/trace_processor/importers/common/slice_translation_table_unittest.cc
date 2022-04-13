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

#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

TEST(SliceTranslationTable, UnknownName) {
  TraceStorage storage;
  SliceTranslationTable table(&storage);
  const StringId raw_name = storage.InternString("name1");
  EXPECT_EQ(raw_name, table.TranslateName(raw_name));
}

TEST(SliceTranslationTable, MappedName) {
  TraceStorage storage;
  SliceTranslationTable table(&storage);
  table.AddNameTranslationRule("raw_name1", "mapped_name1");
  const StringId raw_name = storage.InternString("raw_name1");
  const StringId mapped_name = storage.InternString("mapped_name1");
  EXPECT_EQ(mapped_name, table.TranslateName(raw_name));
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
