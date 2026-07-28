/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/util/flatbuffer_reader.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include "src/trace_processor/util/flatbuffer_writer.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

using W = FlatBufferWriter;

std::vector<uint8_t> Build(W::Offset root, FlatBufferWriter& w) {
  w.Finish(root);
  return w.Release();
}

std::optional<FlatBufferReader> GetRoot(const std::vector<uint8_t>& buf) {
  return FlatBufferReader::GetRoot(buf.data(),
                                   static_cast<uint32_t>(buf.size()));
}

TEST(FlatBufferRoundTripTest, ScalarFields) {
  FlatBufferWriter w;
  w.StartTable();
  w.FieldI32(0, 42);
  w.FieldI16(1, 7);
  w.FieldBool(2, true);
  w.FieldU8(3, 200);
  w.FieldI64(4, 0x123456789ABCDEF0LL);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->Scalar<int32_t>(0), 42);
  EXPECT_EQ(reader->Scalar<int16_t>(1), 7);
  EXPECT_EQ(reader->Scalar<uint8_t>(2), 1);  // bool stored as u8.
  EXPECT_EQ(reader->Scalar<uint8_t>(3), 200);
  EXPECT_EQ(reader->Scalar<int64_t>(4), 0x123456789ABCDEF0LL);
}

// Regression test: EndTable() must survive a buffer grow between prepending
// the soffset placeholder and patching it (the vtable prepend in between can
// reallocate and shift the buffer contents).
TEST(FlatBufferRoundTripTest, GrowDuringEndTable) {
  for (uint32_t capacity = 4; capacity <= 64; capacity += 4) {
    FlatBufferWriter w(capacity);
    w.StartTable();
    for (uint32_t i = 0; i < 24; i++) {
      w.FieldI32(i, static_cast<int32_t>(i * 3));
    }
    auto root = w.EndTable();
    auto buf = Build(root, w);

    auto reader = GetRoot(buf);
    ASSERT_TRUE(reader.has_value()) << "capacity " << capacity;
    for (uint32_t i = 0; i < 24; i++) {
      EXPECT_EQ(reader->Scalar<int32_t>(i), static_cast<int32_t>(i * 3))
          << "capacity " << capacity << " field " << i;
    }
  }
}

TEST(FlatBufferRoundTripTest, AbsentFieldReturnsDefault) {
  FlatBufferWriter w;
  w.StartTable();
  w.FieldI32(1, 55);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->Scalar<int32_t>(0, -1), -1);
  EXPECT_EQ(reader->Scalar<int32_t>(1), 55);
  // Field index well beyond the vtable's extent is also absent.
  EXPECT_EQ(reader->Scalar<int32_t>(50, -7), -7);
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
  w.StartTable();
  w.FieldI32(0, 99);
  auto child = w.EndTable();

  w.StartTable();
  w.FieldOffset(0, child);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  auto child_reader = reader->Table(0);
  ASSERT_TRUE(child_reader);
  EXPECT_EQ(child_reader.Scalar<int32_t>(0), 99);

  // Absent sub-table is an invalid reader.
  EXPECT_FALSE(reader->Table(5));
}

TEST(FlatBufferRoundTripTest, VecTable) {
  FlatBufferWriter w;
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

TEST(FlatBufferRoundTripTest, VecScalar) {
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

TEST(FlatBufferRoundTripTest, EmptyVec) {
  FlatBufferWriter w;
  auto vec = w.WriteVecOffsets(nullptr, 0);
  w.StartTable();
  w.FieldOffset(0, vec);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->VecTable(0).size(), 0u);
  EXPECT_EQ(reader->VecString(0).size(), 0u);
  EXPECT_EQ(reader->VecScalar<int32_t>(0).size(), 0u);
}

TEST(FlatBufferRoundTripTest, EmptyVecStruct) {
  // A zero-element struct vector is the first thing written, so the writer's
  // buffer is still empty when the (empty) element payload is prepended.
  FlatBufferWriter w;
  auto vec = w.WriteVecStruct(nullptr, sizeof(int32_t), 0, alignof(int32_t));
  w.StartTable();
  w.FieldOffset(0, vec);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->VecScalar<int32_t>(0).size(), 0u);
}

TEST(FlatBufferRoundTripTest, VecIndexOutOfBoundsReturnsDefault) {
  // Indexing any vector type at or past size() must return a default value,
  // never read out of bounds. The last valid index still reads correctly.
  int64_t i64s[] = {10, 20};
  double f64s[] = {0.5, 1.5};
  uint8_t u8s[] = {7};
  FlatBufferWriter w;
  auto i64_off = w.WriteVecStruct(i64s, sizeof(int64_t), 2, alignof(int64_t));
  auto f64_off = w.WriteVecStruct(f64s, sizeof(double), 2, alignof(double));
  auto u8_off = w.WriteVecStruct(u8s, sizeof(uint8_t), 1, alignof(uint8_t));
  auto s0 = w.WriteString("only");
  auto str_vec = w.WriteVecOffsets(&s0, 1);

  w.StartTable();
  w.FieldI32(0, 42);
  auto c0 = w.EndTable();
  auto tbl_vec = w.WriteVecOffsets(&c0, 1);

  w.StartTable();
  w.FieldOffset(0, i64_off);
  w.FieldOffset(1, f64_off);
  w.FieldOffset(2, u8_off);
  w.FieldOffset(3, str_vec);
  w.FieldOffset(4, tbl_vec);
  auto root = w.EndTable();
  auto buf = Build(root, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());

  auto i64v = reader->VecScalar<int64_t>(0);
  ASSERT_EQ(i64v.size(), 2u);
  EXPECT_EQ(i64v[1], 20);
  EXPECT_EQ(i64v[2], 0);
  EXPECT_EQ(i64v[0xFFFFFFFF], 0);

  auto f64v = reader->VecScalar<double>(1);
  ASSERT_EQ(f64v.size(), 2u);
  EXPECT_DOUBLE_EQ(f64v[1], 1.5);
  EXPECT_DOUBLE_EQ(f64v[2], 0.0);

  auto u8v = reader->VecScalar<uint8_t>(2);
  ASSERT_EQ(u8v.size(), 1u);
  EXPECT_EQ(u8v[0], 7);
  EXPECT_EQ(u8v[1], 0);

  auto strv = reader->VecString(3);
  ASSERT_EQ(strv.size(), 1u);
  EXPECT_EQ(strv[0], "only");
  EXPECT_EQ(strv[1], "");
  EXPECT_EQ(strv[0xFFFFFFFF], "");

  auto tv = reader->VecTable(4);
  ASSERT_EQ(tv.size(), 1u);
  EXPECT_EQ(tv[0].Scalar<int32_t>(0), 42);
  EXPECT_FALSE(tv[1]);
  EXPECT_FALSE(tv[0xFFFFFFFF]);
}

TEST(FlatBufferRoundTripTest, GetRootTooSmall) {
  uint8_t tiny[] = {0, 0};
  EXPECT_FALSE(FlatBufferReader::GetRoot(tiny, sizeof(tiny)).has_value());
  EXPECT_FALSE(FlatBufferReader::GetRoot(nullptr, 0).has_value());
}

// Simulate a minimal Arrow Schema: Schema { endianness: i16, fields: [Field] }
// where Field { name: string, nullable: bool, type_type: u8 }.
TEST(FlatBufferRoundTripTest, ArrowSchemaLike) {
  FlatBufferWriter w;

  auto name0 = w.WriteString("col_a");
  auto name1 = w.WriteString("col_b");

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

  W::Offset field_offs[] = {field0, field1};
  auto fields_vec = w.WriteVecOffsets(field_offs, 2);

  w.StartTable();
  w.FieldI16(0, 0);  // little-endian
  w.FieldOffset(1, fields_vec);
  auto schema = w.EndTable();

  auto buf = Build(schema, w);

  auto reader = GetRoot(buf);
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->Scalar<int16_t>(0), 0);

  auto fields = reader->VecTable(1);
  ASSERT_EQ(fields.size(), 2u);

  EXPECT_EQ(fields[0].String(0), "col_a");
  EXPECT_EQ(fields[0].Scalar<uint8_t>(1), 0);
  EXPECT_EQ(fields[0].Scalar<uint8_t>(2), 2);

  EXPECT_EQ(fields[1].String(0), "col_b");
  EXPECT_EQ(fields[1].Scalar<uint8_t>(1), 1);
  EXPECT_EQ(fields[1].Scalar<uint8_t>(2), 5);
}

// ---------------------------------------------------------------------------
// Malformed / untrusted input tests.
// ---------------------------------------------------------------------------

TEST(FlatBufferMalformedTest, TruncatedBufferNeverCrashes) {
  FlatBufferWriter w;
  auto name0 = w.WriteString("col_a");
  auto name1 = w.WriteString("col_b");

  w.StartTable();
  w.FieldOffset(0, name0);
  w.FieldBool(1, false);
  w.FieldU8(2, 2);
  auto field0 = w.EndTable();

  w.StartTable();
  w.FieldOffset(0, name1);
  w.FieldBool(1, true);
  w.FieldU8(2, 5);
  auto field1 = w.EndTable();

  W::Offset field_offs[] = {field0, field1};
  auto fields_vec = w.WriteVecOffsets(field_offs, 2);

  w.StartTable();
  w.FieldI16(0, 0);
  w.FieldOffset(1, fields_vec);
  auto schema = w.EndTable();

  auto full = Build(schema, w);

  // Truncate at every possible length and verify accessors return defaults
  // rather than reading out of bounds (ASan/MSan builds will catch OOB
  // accesses; here we also sanity check the returned values are inert).
  for (uint32_t len = 0; len < full.size(); len++) {
    auto reader = FlatBufferReader::GetRoot(full.data(), len);
    if (!reader.has_value()) {
      continue;
    }
    EXPECT_EQ(reader->Scalar<int32_t>(99, -1), -1);
    auto fields = reader->VecTable(1);
    for (uint32_t i = 0; i < fields.size(); i++) {
      auto f = fields[i];
      if (f) {
        f.String(0);
        f.Scalar<uint8_t>(1);
      }
    }
    reader->String(5);
    reader->Table(5);
    reader->VecString(1);
    reader->VecScalar<int32_t>(1);
  }
}

TEST(FlatBufferMalformedTest, VecScalarHugeCountIsRejected) {
  // Hand-craft a buffer: root offset -> table with one vtable field pointing
  // at a "vector" whose declared count is enormous but the buffer itself is
  // tiny. VecScalar must return an empty view, not read out of bounds.
  //
  // Layout (all offsets relative, little endian):
  //   [0..4)   root offset -> table at offset 4
  //   [4..8)   table soffset -> vtable at offset 8 (back from 4: 4-8=-4)
  //   [8..12)  vtable: size=6, table_size=6
  //   [12..14) vtable slot for field 0: offset 6 (points to table+6=10)
  //   table field data at offset 10: u32 offset to the "vector"
  //   vector location: count = 0xFFFFFFFF, no actual elements follow.
  std::vector<uint8_t> buf(24, 0);
  auto put_u32 = [&](uint32_t pos, uint32_t v) {
    memcpy(buf.data() + pos, &v, 4);
  };
  auto put_u16 = [&](uint32_t pos, uint16_t v) {
    memcpy(buf.data() + pos, &v, 2);
  };
  auto put_i32 = [&](uint32_t pos, int32_t v) {
    memcpy(buf.data() + pos, &v, 4);
  };

  put_u32(0, 4);            // root offset: table at 4.
  put_i32(4, -4);           // table soffset: vtable at 4-(-4)=8.
  put_u16(8, 6);            // vtable_size.
  put_u16(10, 6);           // table_size.
  put_u16(12, 6);           // slot 0 -> table + 6 = offset 10.
  put_u32(10, 20 - 10);     // field: relative offset to "vector" at 20.
  put_u32(20, 0xFFFFFFFF);  // vector count: enormous, buffer ends at 24.

  auto reader =
      FlatBufferReader::GetRoot(buf.data(), static_cast<uint32_t>(buf.size()));
  ASSERT_TRUE(reader.has_value());
  auto sv = reader->VecScalar<int32_t>(0);
  EXPECT_EQ(sv.size(), 0u);
  auto tv = reader->VecTable(0);
  EXPECT_EQ(tv.size(), 0u);
  auto strv = reader->VecString(0);
  EXPECT_EQ(strv.size(), 0u);
}

TEST(FlatBufferMalformedTest, OffsetPastEndIsRejected) {
  // Same skeleton as above, but the field holds an offset that resolves
  // past the end of the buffer entirely.
  std::vector<uint8_t> buf(16, 0);
  auto put_u32 = [&](uint32_t pos, uint32_t v) {
    memcpy(buf.data() + pos, &v, 4);
  };
  auto put_u16 = [&](uint32_t pos, uint16_t v) {
    memcpy(buf.data() + pos, &v, 2);
  };
  auto put_i32 = [&](uint32_t pos, int32_t v) {
    memcpy(buf.data() + pos, &v, 4);
  };

  put_u32(0, 4);
  put_i32(4, -4);
  put_u16(8, 6);
  put_u16(10, 6);
  put_u16(12, 6);
  // Field holds a relative offset that points miles past the 16-byte buffer.
  put_u32(10, 0x7FFFFFFF);

  auto reader =
      FlatBufferReader::GetRoot(buf.data(), static_cast<uint32_t>(buf.size()));
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->String(0), "");
  EXPECT_FALSE(reader->Table(0));
  EXPECT_EQ(reader->VecScalar<int32_t>(0).size(), 0u);
  EXPECT_EQ(reader->VecTable(0).size(), 0u);
  EXPECT_EQ(reader->VecString(0).size(), 0u);
}

}  // namespace
}  // namespace perfetto::trace_processor::util
