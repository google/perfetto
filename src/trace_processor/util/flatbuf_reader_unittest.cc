/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/util/flatbuf_reader.h"
#include "src/trace_processor/util/flatbuf_writer.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

using W = FlatBufferWriter;

// Helper: build a flatbuffer, return the bytes.
std::vector<uint8_t> Build(W::Offset root, FlatBufferWriter& w) {
  w.Finish(root);
  return w.Release();
}

// Helper: GetRoot with automatic size_t -> uint32_t cast.
std::optional<FlatBufferReader> GetRoot(const std::vector<uint8_t>& buf) {
  return FlatBufferReader::GetRoot(buf.data(),
                                   static_cast<uint32_t>(buf.size()));
}

TEST(FlatBufferRoundTripTest, ScalarFields) {
  // Table with field 0 = i32(42), field 1 = i16(7), field 2 = bool(true).
  FlatBufferWriter w;
  w.StartTable();
  w.FieldI32(0, 42);
  w.FieldI16(1, 7);
  w.FieldBool(2, true);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->Scalar<int32_t>(0), 42);
  EXPECT_EQ(reader->Scalar<int16_t>(1), 7);
  EXPECT_EQ(reader->Scalar<uint8_t>(2), 1);  // bool stored as u8
}

TEST(FlatBufferRoundTripTest, StringField) {
  FlatBufferWriter w;
  auto hello = w.WriteString("hello");
  w.StartTable();
  w.FieldOffset(0, hello);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->String(0), "hello");
}

TEST(FlatBufferRoundTripTest, SubTable) {
  FlatBufferWriter w;

  // Build child table: field 0 = i32(99).
  w.StartTable();
  w.FieldI32(0, 99);
  auto child = w.EndTable();

  // Build root table: field 0 = child table.
  w.StartTable();
  w.FieldOffset(0, child);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  auto child_reader = reader->Table(0);
  ASSERT_TRUE(child_reader);
  EXPECT_EQ(child_reader.Scalar<int32_t>(0), 99);
}

TEST(FlatBufferRoundTripTest, VecTable) {
  FlatBufferWriter w;

  // Build two child tables.
  w.StartTable();
  w.FieldI32(0, 10);
  auto c0 = w.EndTable();

  w.StartTable();
  w.FieldI32(0, 20);
  auto c1 = w.EndTable();

  W::Offset offs[] = {c0, c1};
  auto vec = w.WriteVecOffsets(offs, 2);

  w.StartTable();
  w.FieldOffset(0, vec);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  auto tv = reader->VecTable(0);
  ASSERT_EQ(tv.size(), 2u);
  EXPECT_EQ(tv[0].Scalar<int32_t>(0), 10);
  EXPECT_EQ(tv[1].Scalar<int32_t>(0), 20);
}

TEST(FlatBufferRoundTripTest, VecString) {
  FlatBufferWriter w;
  auto s0 = w.WriteString("alpha");
  auto s1 = w.WriteString("beta");
  W::Offset offs[] = {s0, s1};
  auto vec = w.WriteVecOffsets(offs, 2);

  w.StartTable();
  w.FieldOffset(0, vec);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  auto sv = reader->VecString(0);
  ASSERT_EQ(sv.size(), 2u);
  EXPECT_EQ(sv[0], "alpha");
  EXPECT_EQ(sv[1], "beta");
}

TEST(FlatBufferRoundTripTest, AbsentField) {
  // Table with only field 1 set; field 0 is absent.
  FlatBufferWriter w;
  w.StartTable();
  w.FieldI32(1, 55);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->Scalar<int32_t>(0, -1), -1);  // default
  EXPECT_EQ(reader->Scalar<int32_t>(1), 55);
}

TEST(FlatBufferRoundTripTest, EmptyVec) {
  FlatBufferWriter w;
  auto vec = w.WriteVecOffsets(nullptr, 0);
  w.StartTable();
  w.FieldOffset(0, vec);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  auto tv = reader->VecTable(0);
  EXPECT_EQ(tv.size(), 0u);
}

TEST(FlatBufferRoundTripTest, I64Field) {
  FlatBufferWriter w;
  w.StartTable();
  w.FieldI64(0, 0x123456789ABCDEF0LL);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->Scalar<int64_t>(0), 0x123456789ABCDEF0LL);
}

TEST(FlatBufferRoundTripTest, VecScalar) {
  // Write a vector of int32 scalars using WriteVecStruct (struct of size 4).
  int32_t vals[] = {10, 20, 30};
  FlatBufferWriter w;
  auto vec_off = w.WriteVecStruct(vals, sizeof(int32_t), 3, alignof(int32_t));
  w.StartTable();
  w.FieldOffset(0, vec_off);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  auto sv = reader->VecScalar<int32_t>(0);
  ASSERT_EQ(sv.size(), 3u);
  EXPECT_EQ(sv[0], 10);
  EXPECT_EQ(sv[1], 20);
  EXPECT_EQ(sv[2], 30);
}

TEST(FlatBufferRoundTripTest, GetRootTooSmall) {
  uint8_t tiny[] = {0, 0};
  auto r = FlatBufferReader::GetRoot(tiny, sizeof(tiny));
  EXPECT_FALSE(r.has_value());
}

// Simulate a minimal Arrow Schema: Schema { endianness: i16, fields: [Field] }
// where Field { name: string, nullable: bool, type_type: u8 }.
TEST(FlatBufferRoundTripTest, ArrowSchemaLike) {
  FlatBufferWriter w;

  // Write field name strings.
  auto name0 = w.WriteString("col_a");
  auto name1 = w.WriteString("col_b");

  // Build Field tables.
  // Field layout: field 0 = name (offset), field 1 = nullable (bool),
  //               field 2 = type_type (u8).
  w.StartTable();
  w.FieldOffset(0, name0);
  w.FieldBool(1, false);
  w.FieldU8(2, 2);  // kTypeInt
  auto field0 = w.EndTable();

  w.StartTable();
  w.FieldOffset(0, name1);
  w.FieldBool(1, true);
  w.FieldU8(2, 5);  // kTypeUtf8
  auto field1 = w.EndTable();

  // Fields vector.
  W::Offset field_offs[] = {field0, field1};
  auto fields_vec = w.WriteVecOffsets(field_offs, 2);

  // Schema table: field 0 = endianness (i16), field 1 = fields (offset).
  w.StartTable();
  w.FieldI16(0, 0);  // little-endian
  w.FieldOffset(1, fields_vec);
  auto schema = w.EndTable();

  auto buf = Build(schema, w);

  // Now read it back.
  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());

  EXPECT_EQ(reader->Scalar<int16_t>(0), 0);  // endianness

  auto fields = reader->VecTable(1);
  ASSERT_EQ(fields.size(), 2u);

  EXPECT_EQ(fields[0].String(0), "col_a");
  EXPECT_EQ(fields[0].Scalar<uint8_t>(1), 0);  // nullable = false
  EXPECT_EQ(fields[0].Scalar<uint8_t>(2), 2);  // type_type = Int

  EXPECT_EQ(fields[1].String(0), "col_b");
  EXPECT_EQ(fields[1].Scalar<uint8_t>(1), 1);  // nullable = true
  EXPECT_EQ(fields[1].Scalar<uint8_t>(2), 5);  // type_type = Utf8
}

}  // namespace
}  // namespace perfetto::trace_processor::util
