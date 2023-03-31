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

#include "src/trace_processor/importers/common/deobfuscation_mapping_table.h"
#include <string>
#include "perfetto/ext/base/flat_hash_map.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

using PackageId = DeobfuscationMappingTable::PackageId;

TEST(DeobfuscationMappingTable, EmptyTableByDefault) {
  TraceStorage storage;
  StringId xyz_id = storage.InternString("xyz");

  DeobfuscationMappingTable table;
  EXPECT_EQ(table.TranslateClass(xyz_id), std::nullopt);
  EXPECT_EQ(table.TranslateClass(PackageId{"app", 123}, xyz_id), std::nullopt);
}

TEST(DeobfuscationMappingTable, TranslateClassSingleInsert) {
  TraceStorage storage;
  StringId xyz_id = storage.InternString("xyz");
  StringId abc_id = storage.InternString("abc");
  StringId class_x_id = storage.InternString("class_X");

  DeobfuscationMappingTable table;
  table.AddClassTranslation(PackageId{"app", 123}, xyz_id, class_x_id,
                            base::FlatHashMap<StringId, StringId>{});
  EXPECT_EQ(table.TranslateClass(xyz_id), class_x_id);
  EXPECT_EQ(table.TranslateClass(PackageId{"app", 123}, xyz_id), class_x_id);
  EXPECT_EQ(table.TranslateClass(PackageId{"app", 124}, xyz_id), std::nullopt);
  EXPECT_EQ(table.TranslateClass(PackageId{"app", 123}, abc_id), std::nullopt);
}

TEST(DeobfuscationMappingTable, TranslateClassMultipleInsert) {
  TraceStorage storage;
  StringId xyz_id = storage.InternString("xyz");
  StringId abc_id = storage.InternString("abc");
  StringId class_x_id = storage.InternString("class_X");
  StringId class_y_id = storage.InternString("class_Y");
  StringId class_a_id = storage.InternString("class_A");

  DeobfuscationMappingTable table;
  table.AddClassTranslation(PackageId{"app1", 123}, xyz_id, class_x_id,
                            base::FlatHashMap<StringId, StringId>{});
  table.AddClassTranslation(PackageId{"app2", 123}, xyz_id, class_y_id,
                            base::FlatHashMap<StringId, StringId>{});
  table.AddClassTranslation(PackageId{"app3", 123}, abc_id, class_a_id,
                            base::FlatHashMap<StringId, StringId>{});
  EXPECT_EQ(table.TranslateClass(xyz_id), class_x_id);
  EXPECT_EQ(table.TranslateClass(abc_id), std::nullopt);
  EXPECT_EQ(table.TranslateClass(PackageId{"app1", 123}, xyz_id), class_x_id);
  EXPECT_EQ(table.TranslateClass(PackageId{"app2", 123}, xyz_id), class_y_id);
  EXPECT_EQ(table.TranslateClass(PackageId{"app1", 123}, abc_id), std::nullopt);
}

TEST(DeobfuscationMappingTable, TranslateMember) {
  TraceStorage storage;
  StringId xyz_id = storage.InternString("xyz");
  StringId abc_id = storage.InternString("abc");
  StringId class_x_id = storage.InternString("class_X");
  StringId mmm_1_id = storage.InternString("mmm1");
  StringId mmm_2_id = storage.InternString("mmm2");
  StringId mmm_3_id = storage.InternString("mmm3");
  StringId mmm_4_id = storage.InternString("mmm4");
  StringId member_1_id = storage.InternString("member_1");
  StringId member_2_id = storage.InternString("member_2");
  StringId member_3_id = storage.InternString("member_3");

  base::FlatHashMap<StringId, StringId> members;
  members[mmm_1_id] = member_1_id;
  members[mmm_2_id] = member_2_id;
  members[mmm_3_id] = member_3_id;
  DeobfuscationMappingTable table;
  table.AddClassTranslation(PackageId{"app1", 123}, xyz_id, class_x_id,
                            std::move(members));
  EXPECT_EQ(table.TranslateMember(PackageId{"app1", 123}, xyz_id, mmm_2_id),
            member_2_id);
  EXPECT_EQ(table.TranslateMember(PackageId{"app1", 123}, xyz_id, mmm_4_id),
            std::nullopt);
  EXPECT_EQ(table.TranslateMember(PackageId{"app1", 123}, abc_id, mmm_2_id),
            std::nullopt);
  EXPECT_EQ(table.TranslateMember(PackageId{"app1", 124}, xyz_id, mmm_2_id),
            std::nullopt);
}

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
