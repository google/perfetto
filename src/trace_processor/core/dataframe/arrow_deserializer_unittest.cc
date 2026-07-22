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

#include "src/trace_processor/core/dataframe/arrow_deserializer.h"
#include "src/trace_processor/core/dataframe/arrow_test_utils.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/typed_cursor.h"
#include "src/trace_processor/util/flatbuffer_reader.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

using arrow_test::Deserialize;
using arrow_test::MakeDataframe;
using arrow_test::RoundTrip;
using arrow_test::Serialize;
using perfetto::base::gtest_matchers::IsError;

struct TestFieldNode {
  int64_t length;
  int64_t null_count;
};

struct TestArrowBuffer {
  int64_t offset;
  int64_t length;
};

struct TestBlock {
  int64_t offset;
  int32_t metadata_length;
  int32_t padding;
  int64_t body_length;
};

uint8_t* FollowMutableOffset(std::vector<uint8_t>* bytes,
                             const uint8_t* offset_location) {
  uint32_t relative_offset;
  memcpy(&relative_offset, offset_location, sizeof(relative_offset));
  size_t location = static_cast<size_t>(offset_location - bytes->data());
  return bytes->data() + location + relative_offset;
}

uint8_t* MutableVectorData(std::vector<uint8_t>* bytes,
                           const util::FlatBufferReader& table,
                           uint32_t field) {
  const uint8_t* vector_offset = table.FieldRaw(field, sizeof(uint32_t));
  EXPECT_NE(vector_offset, nullptr);
  uint8_t* vector = FollowMutableOffset(bytes, vector_offset);
  uint32_t count;
  memcpy(&count, vector, sizeof(count));
  EXPECT_GT(count, 0u);
  return vector + sizeof(count);
}

size_t FooterOffset(const std::vector<uint8_t>& bytes) {
  uint32_t footer_size;
  memcpy(&footer_size, bytes.data() + bytes.size() - 10, sizeof(footer_size));
  return bytes.size() - 10 - footer_size;
}

TestBlock ReadBlock(const std::vector<uint8_t>& bytes) {
  size_t footer_offset = FooterOffset(bytes);
  uint32_t footer_size =
      static_cast<uint32_t>(bytes.size() - 10 - footer_offset);
  auto footer = util::FlatBufferReader::GetRoot(bytes.data() + footer_offset,
                                                footer_size);
  EXPECT_TRUE(footer.has_value());
  auto blocks = footer->VecScalar<TestBlock>(3);
  EXPECT_EQ(blocks.size(), 1u);
  return blocks[0];
}

uint8_t* MutableBlock(std::vector<uint8_t>* bytes) {
  size_t footer_offset = FooterOffset(*bytes);
  uint32_t footer_size =
      static_cast<uint32_t>(bytes->size() - 10 - footer_offset);
  auto footer = util::FlatBufferReader::GetRoot(bytes->data() + footer_offset,
                                                footer_size);
  EXPECT_TRUE(footer.has_value());
  return MutableVectorData(bytes, *footer, 3);
}

util::FlatBufferReader MutableRecordBatch(std::vector<uint8_t>* bytes) {
  TestBlock block = ReadBlock(*bytes);
  uint32_t metadata_size;
  memcpy(&metadata_size, bytes->data() + block.offset + 4,
         sizeof(metadata_size));
  auto message = util::FlatBufferReader::GetRoot(
      bytes->data() + block.offset + 8, metadata_size);
  EXPECT_TRUE(message.has_value());
  auto record_batch = message->Table(2);
  EXPECT_TRUE(static_cast<bool>(record_batch));
  return record_batch;
}

inline constexpr auto kUint32NonNull = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Uint32{}, NonNull{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripUint32NonNull) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{10}, uint32_t{20},
                           uint32_t{30});
  auto dst = RoundTrip(kUint32NonNull, src, &pool);

  ASSERT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32NonNull, 0)), 10u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32NonNull, 1)), 20u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32NonNull, 2)), 30u);
}

TEST(ArrowDeserializerTest, RoundTripFinalizedSource) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{7}, uint32_t{8});
  src.Finalize();
  auto dst = RoundTrip(kUint32NonNull, src, &pool);

  ASSERT_EQ(dst.row_count(), 2u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32NonNull, 0)), 7u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32NonNull, 1)), 8u);
}

TEST(ArrowDeserializerTest, RoundTripChunkedInput) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1}, uint32_t{2},
                           uint32_t{3}, uint32_t{4});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  auto dst_or = Deserialize(bytes, &pool, src.CreateSpec(), /*chunk_size=*/7);
  ASSERT_OK(dst_or);
  Dataframe dst = std::move(*dst_or);
  ASSERT_EQ(dst.row_count(), 4u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32NonNull, 3)), 4u);
}

TEST(ArrowDeserializerTest, RoundTripNonZeroStartOffset) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{42});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  // Simulate a reader whose window does not start at file offset zero, as
  // happens when the snapshot importer slices a member out of a tar stream.
  util::TraceBlobViewReader reader;
  std::vector<uint8_t> padding(123, 0xAB);
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(padding.data(), padding.size())));
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));
  ASSERT_TRUE(reader.PopFrontUntil(padding.size()));

  auto dst_or = DeserializeFromArrow(reader, &pool, src.CreateSpec());
  ASSERT_OK(dst_or);
  Dataframe dst = std::move(*dst_or);
  ASSERT_EQ(dst.row_count(), 1u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32NonNull, 0)), 42u);
}

inline constexpr auto kUint32DenseNull = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Uint32{}, DenseNull{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripUint32DenseNull) {
  StringPool pool;
  auto src =
      MakeDataframe(kUint32DenseNull, &pool, std::optional<uint32_t>{10},
                    std::optional<uint32_t>{}, std::optional<uint32_t>{30},
                    std::optional<uint32_t>{}, std::optional<uint32_t>{50});
  auto dst = RoundTrip(kUint32DenseNull, src, &pool);

  ASSERT_EQ(dst.row_count(), 5u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNull, 0)),
            std::optional<uint32_t>{10});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNull, 1)),
            std::optional<uint32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNull, 2)),
            std::optional<uint32_t>{30});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNull, 3)),
            std::optional<uint32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNull, 4)),
            std::optional<uint32_t>{50});
}

inline constexpr auto kInt64SparsePopcountAlways = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int64{}, SparseNullWithPopcountAlways{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripInt64SparseNullPopcountAlways) {
  StringPool pool;
  auto src = MakeDataframe(
      kInt64SparsePopcountAlways, &pool, std::optional<int64_t>{-1000000000000},
      std::optional<int64_t>{}, std::optional<int64_t>{1000000000000});
  auto dst = RoundTrip(kInt64SparsePopcountAlways, src, &pool);

  ASSERT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64SparsePopcountAlways, 0)),
            std::optional<int64_t>{-1000000000000});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64SparsePopcountAlways, 1)),
            std::optional<int64_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64SparsePopcountAlways, 2)),
            std::optional<int64_t>{1000000000000});
}

// Exercises the prefix-popcount reconstruction across 64-bit word boundaries.
TEST(ArrowDeserializerTest, RoundTripSparseNullAcrossWordBoundary) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt64SparsePopcountAlways, &pool);
  for (int64_t i = 0; i < 200; i++) {
    src.InsertUnchecked(kInt64SparsePopcountAlways, std::monostate{},
                        i % 3 == 0 ? std::optional<int64_t>{}
                                   : std::optional<int64_t>{i * 100});
  }
  auto dst = RoundTrip(kInt64SparsePopcountAlways, src, &pool);

  ASSERT_EQ(dst.row_count(), 200u);
  for (uint32_t i = 0; i < 200; i++) {
    auto expected = i % 3 == 0 ? std::optional<int64_t>{}
                               : std::optional<int64_t>{int64_t{i} * 100};
    EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64SparsePopcountAlways, i)),
              expected)
        << "row " << i;
  }
}

inline constexpr auto kInt32SparseUntilFinalization = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int32{},
                          SparseNullWithPopcountUntilFinalization{},
                          Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripInt32SparseNullPopcountUntilFinalization) {
  StringPool pool;
  auto src = MakeDataframe(kInt32SparseUntilFinalization, &pool,
                           std::optional<int32_t>{-5}, std::optional<int32_t>{},
                           std::optional<int32_t>{99});
  auto dst = RoundTrip(kInt32SparseUntilFinalization, src, &pool);

  ASSERT_TRUE(dst.finalized());
  EXPECT_EQ(Serialize(dst, pool), Serialize(src, pool));
}

inline constexpr auto kInt32Sparse = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int32{}, SparseNull{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripPlainSparseNull) {
  StringPool pool;
  auto src =
      MakeDataframe(kInt32Sparse, &pool, std::optional<int32_t>{},
                    std::optional<int32_t>{-2}, std::optional<int32_t>{});
  std::vector<uint8_t> serialized = Serialize(src, pool);
  auto dst_or = Deserialize(serialized, &pool, src.CreateSpec());
  ASSERT_OK(dst_or);
  Dataframe dst = std::move(*dst_or);

  // Plain SparseNull deliberately has no random-access API. Serializing again
  // verifies its physical storage and validity bitmap instead.
  EXPECT_EQ(Serialize(dst, pool), serialized);
}

TEST(ArrowDeserializerTest, RoundTripEmptySparseColumn) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt32Sparse, &pool);
  auto dst = RoundTrip(kInt32Sparse, src, &pool);
  EXPECT_EQ(dst.row_count(), 0u);
}

TEST(ArrowDeserializerTest, RoundTripAllNullSparseColumn) {
  StringPool pool;
  auto src =
      MakeDataframe(kInt64SparsePopcountAlways, &pool, std::optional<int64_t>{},
                    std::optional<int64_t>{}, std::optional<int64_t>{});
  auto dst = RoundTrip(kInt64SparsePopcountAlways, src, &pool);

  ASSERT_EQ(dst.row_count(), 3u);
  for (uint32_t row = 0; row < dst.row_count(); ++row) {
    EXPECT_FALSE(
        (dst.GetCellUnchecked<1>(kInt64SparsePopcountAlways, row)).has_value());
  }
}

inline constexpr auto kDoubleDenseNull = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Double{}, DenseNull{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripDoubleDenseNull) {
  StringPool pool;
  auto src = MakeDataframe(kDoubleDenseNull, &pool, std::optional<double>{1.5},
                           std::optional<double>{}, std::optional<double>{3.0});
  auto dst = RoundTrip(kDoubleDenseNull, src, &pool);

  ASSERT_EQ(dst.row_count(), 3u);
  auto v0 = dst.GetCellUnchecked<1>(kDoubleDenseNull, 0);
  auto v1 = dst.GetCellUnchecked<1>(kDoubleDenseNull, 1);
  auto v2 = dst.GetCellUnchecked<1>(kDoubleDenseNull, 2);
  ASSERT_TRUE(v0.has_value());
  EXPECT_DOUBLE_EQ(*v0, 1.5);
  EXPECT_FALSE(v1.has_value());
  ASSERT_TRUE(v2.has_value());
  EXPECT_DOUBLE_EQ(*v2, 3.0);
}

inline constexpr auto kStringNonNull = CreateTypedDataframeSpec(
    {"_auto_id", "name"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripStringNonNull) {
  StringPool pool;
  auto hello = pool.InternString(base::StringView("hello"));
  auto world = pool.InternString(base::StringView("world"));
  auto empty = pool.InternString(base::StringView(""));
  auto src = MakeDataframe(kStringNonNull, &pool, hello, world, empty);

  // Deserialize into a separate pool to prove strings travel by value.
  std::vector<uint8_t> bytes = Serialize(src, pool);
  StringPool other_pool;
  auto dst_or = Deserialize(bytes, &other_pool, src.CreateSpec());
  ASSERT_OK(dst_or);
  Dataframe dst = std::move(*dst_or);

  ASSERT_EQ(dst.row_count(), 3u);
  EXPECT_EQ(
      other_pool.Get(dst.GetCellUnchecked<1>(kStringNonNull, 0)).ToStdString(),
      "hello");
  EXPECT_EQ(
      other_pool.Get(dst.GetCellUnchecked<1>(kStringNonNull, 1)).ToStdString(),
      "world");
  EXPECT_EQ(
      other_pool.Get(dst.GetCellUnchecked<1>(kStringNonNull, 2)).ToStdString(),
      "");
}

TEST(ArrowDeserializerTest, RoundTripUnalignedInput) {
  StringPool pool;
  auto value = pool.InternString(base::StringView("unaligned"));
  auto src = MakeDataframe(kStringNonNull, &pool, value);
  std::vector<uint8_t> bytes = Serialize(src, pool);

  // Keep the prefix and file in one blob so every Arrow buffer starts at an
  // address which is deliberately not naturally aligned.
  std::vector<uint8_t> combined(1, 0xAB);
  combined.insert(combined.end(), bytes.begin(), bytes.end());
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(combined.data(), combined.size())));
  ASSERT_TRUE(reader.PopFrontBytes(1));

  StringPool output_pool;
  auto dst_or = DeserializeFromArrow(reader, &output_pool, src.CreateSpec());
  ASSERT_OK(dst_or);
  Dataframe dst = std::move(*dst_or);
  EXPECT_EQ(
      output_pool.Get(dst.GetCellUnchecked<1>(kStringNonNull, 0)).ToStdString(),
      "unaligned");
}

inline constexpr auto kStringSparse = CreateTypedDataframeSpec(
    {"_auto_id", "name"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{},
                          SparseNullWithPopcountAlways{},
                          Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripStringSparseNull) {
  StringPool pool;
  auto hello = pool.InternString(base::StringView("hello"));
  auto world = pool.InternString(base::StringView("world"));
  auto src = MakeDataframe(
      kStringSparse, &pool, std::optional<StringPool::Id>{hello},
      std::optional<StringPool::Id>{}, std::optional<StringPool::Id>{world});
  auto dst = RoundTrip(kStringSparse, src, &pool);

  ASSERT_EQ(dst.row_count(), 3u);
  auto v0 = dst.GetCellUnchecked<1>(kStringSparse, 0);
  auto v1 = dst.GetCellUnchecked<1>(kStringSparse, 1);
  auto v2 = dst.GetCellUnchecked<1>(kStringSparse, 2);
  ASSERT_TRUE(v0.has_value());
  EXPECT_EQ(pool.Get(*v0).ToStdString(), "hello");
  EXPECT_FALSE(v1.has_value());
  ASSERT_TRUE(v2.has_value());
  EXPECT_EQ(pool.Get(*v2).ToStdString(), "world");
}

inline constexpr auto kStringDenseNull = CreateTypedDataframeSpec(
    {"_auto_id", "name"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{}, DenseNull{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripStringDenseNull) {
  StringPool pool;
  auto hello = pool.InternString(base::StringView("hello"));
  auto src = MakeDataframe(kStringDenseNull, &pool,
                           std::optional<StringPool::Id>{hello},
                           std::optional<StringPool::Id>{});
  auto dst = RoundTrip(kStringDenseNull, src, &pool);

  ASSERT_EQ(dst.row_count(), 2u);
  auto v0 = dst.GetCellUnchecked<1>(kStringDenseNull, 0);
  ASSERT_TRUE(v0.has_value());
  EXPECT_EQ(pool.Get(*v0).ToStdString(), "hello");
  EXPECT_FALSE((dst.GetCellUnchecked<1>(kStringDenseNull, 1)).has_value());
}

inline constexpr auto kMultiColumn = CreateTypedDataframeSpec(
    {"_auto_id", "ts", "name", "value"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int64{}, NonNull{}, Sorted{}),
    CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}),
    CreateTypedColumnSpec(Double{}, DenseNull{}, Unsorted{}));

TEST(ArrowDeserializerTest, RoundTripMultiColumn) {
  StringPool pool;
  auto a = pool.InternString(base::StringView("a"));
  auto bb = pool.InternString(base::StringView("bb"));
  auto src = Dataframe::CreateFromTypedSpec(kMultiColumn, &pool);
  src.InsertUnchecked(kMultiColumn, std::monostate{}, int64_t{100}, a,
                      std::optional<double>{0.5});
  src.InsertUnchecked(kMultiColumn, std::monostate{}, int64_t{200}, bb,
                      std::optional<double>{});
  auto dst = RoundTrip(kMultiColumn, src, &pool);

  ASSERT_EQ(dst.row_count(), 2u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kMultiColumn, 1)), 200);
  EXPECT_EQ(pool.Get(dst.GetCellUnchecked<2>(kMultiColumn, 1)).ToStdString(),
            "bb");
  EXPECT_FALSE((dst.GetCellUnchecked<3>(kMultiColumn, 1)).has_value());
}

TEST(ArrowDeserializerTest, RoundTripEmptyDataframe) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kMultiColumn, &pool);
  auto dst = RoundTrip(kMultiColumn, src, &pool);
  EXPECT_EQ(dst.row_count(), 0u);
}

inline constexpr auto kIdOnly = CreateTypedDataframeSpec(
    {"_auto_id"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}));

TEST(ArrowDeserializerTest, RoundTripIdOnlyDataframe) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kIdOnly, &pool);
  src.InsertUnchecked(kIdOnly, std::monostate{});
  src.InsertUnchecked(kIdOnly, std::monostate{});
  src.InsertUnchecked(kIdOnly, std::monostate{});
  auto dst = RoundTrip(kIdOnly, src, &pool);

  ASSERT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<0>(kIdOnly, 0)), 0u);
  EXPECT_EQ((dst.GetCellUnchecked<0>(kIdOnly, 2)), 2u);
}

// --- Error handling on malformed input -------------------------------------

TEST(ArrowDeserializerTest, ReturnsFinalizedDataframe) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  auto dst_or = Deserialize(bytes, &pool, src.CreateSpec());
  ASSERT_OK(dst_or);
  Dataframe dst = std::move(*dst_or);
  EXPECT_TRUE(dst.finalized());
}

TEST(ArrowDeserializerTest, ReturnedDataframeSupportsIndexesAndCursors) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{20}, uint32_t{10});
  std::vector<uint8_t> bytes = Serialize(src, pool);
  auto dst_or = Deserialize(bytes, &pool, src.CreateSpec());
  ASSERT_OK(dst_or);
  Dataframe dst = std::move(*dst_or);

  uint32_t indexed_column = 1;
  auto index = dst.BuildIndex(&indexed_column, &indexed_column + 1);
  ASSERT_OK(index);
  Dataframe indexed = dst.AddIndex(std::move(*index));
  TypedCursor cursor(
      &indexed, std::vector<FilterSpec>{},
      std::vector<SortSpec>{{indexed_column, SortDirection::kAscending}});
  cursor.ExecuteUnchecked();
  ASSERT_FALSE(cursor.Eof());
  EXPECT_EQ(cursor.GetCellUnchecked<1>(kUint32NonNull), 10u);
  cursor.Next();
  ASSERT_FALSE(cursor.Eof());
  EXPECT_EQ(cursor.GetCellUnchecked<1>(kUint32NonNull), 20u);
  cursor.Next();
  EXPECT_TRUE(cursor.Eof());
}

TEST(ArrowDeserializerTest, RejectsInvalidDataframeSpec) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(src, pool);
  DataframeSpec invalid_spec{{"only_a_name"}, {}};

  EXPECT_THAT(Deserialize(bytes, &pool, invalid_spec), IsError());
}

TEST(ArrowDeserializerTest, RejectsTruncatedInput) {
  StringPool pool;
  auto src = MakeDataframe(kStringSparse, &pool,
                           std::optional<StringPool::Id>{pool.InternString(
                               base::StringView("some string data"))},
                           std::optional<StringPool::Id>{});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  for (size_t len = 0; len < bytes.size(); len += 7) {
    std::vector<uint8_t> truncated(bytes.begin(),
                                   bytes.begin() + static_cast<ptrdiff_t>(len));
    auto dst = Dataframe::CreateFromTypedSpec(kStringSparse, &pool);
    EXPECT_THAT(Deserialize(truncated, &pool, dst.CreateSpec()), IsError())
        << "len " << len;
  }
}

TEST(ArrowDeserializerTest, RejectsCorruptFooterSize) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  uint32_t huge = 0x7FFFFFFF;
  memcpy(bytes.data() + bytes.size() - 10, &huge, 4);
  auto dst = Dataframe::CreateFromTypedSpec(kUint32NonNull, &pool);
  EXPECT_THAT(Deserialize(bytes, &pool, dst.CreateSpec()), IsError());
}

TEST(ArrowDeserializerTest, RejectsInconsistentBlockLength) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  TestBlock block;
  memcpy(&block, MutableBlock(&bytes), sizeof(block));
  block.body_length += 8;
  memcpy(MutableBlock(&bytes), &block, sizeof(block));

  auto dst = Dataframe::CreateFromTypedSpec(kUint32NonNull, &pool);
  EXPECT_THAT(Deserialize(bytes, &pool, dst.CreateSpec()), IsError());
}

TEST(ArrowDeserializerTest, RejectsInconsistentFieldNode) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  auto record_batch = MutableRecordBatch(&bytes);
  uint8_t* node_data = MutableVectorData(&bytes, record_batch, 1);
  TestFieldNode node;
  memcpy(&node, node_data, sizeof(node));
  node.null_count = 1;
  memcpy(node_data, &node, sizeof(node));

  auto dst = Dataframe::CreateFromTypedSpec(kUint32NonNull, &pool);
  EXPECT_THAT(Deserialize(bytes, &pool, dst.CreateSpec()), IsError());
}

TEST(ArrowDeserializerTest, RejectsInvalidEmptyValidityBufferOffset) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  auto record_batch = MutableRecordBatch(&bytes);
  uint8_t* buffer_data = MutableVectorData(&bytes, record_batch, 2);
  TestArrowBuffer buffer;
  memcpy(&buffer, buffer_data, sizeof(buffer));
  ASSERT_EQ(buffer.length, 0);
  buffer.offset = -1;
  memcpy(buffer_data, &buffer, sizeof(buffer));

  auto dst = Dataframe::CreateFromTypedSpec(kUint32NonNull, &pool);
  EXPECT_THAT(Deserialize(bytes, &pool, dst.CreateSpec()), IsError());
}

TEST(ArrowDeserializerTest, RejectsInvalidStringOffsets) {
  StringPool pool;
  auto value = pool.InternString(base::StringView("value"));
  auto src = MakeDataframe(kStringNonNull, &pool, value);
  std::vector<uint8_t> bytes = Serialize(src, pool);

  TestBlock block = ReadBlock(bytes);
  auto record_batch = MutableRecordBatch(&bytes);
  uint8_t* buffer_data = MutableVectorData(&bytes, record_batch, 2);
  TestArrowBuffer offsets_buffer;
  memcpy(&offsets_buffer, buffer_data + sizeof(TestArrowBuffer),
         sizeof(offsets_buffer));
  int32_t invalid_offset = -1;
  memcpy(bytes.data() + block.offset + block.metadata_length +
             offsets_buffer.offset + sizeof(int32_t),
         &invalid_offset, sizeof(invalid_offset));

  auto dst = Dataframe::CreateFromTypedSpec(kStringNonNull, &pool);
  EXPECT_THAT(Deserialize(bytes, &pool, dst.CreateSpec()), IsError());
}

TEST(ArrowDeserializerTest, RejectsSchemaShapeMismatch) {
  StringPool pool;
  auto src = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(src, pool);

  // A dataframe with a different column shape must be rejected via the
  // node/buffer count check.
  auto dst = Dataframe::CreateFromTypedSpec(kMultiColumn, &pool);
  EXPECT_THAT(Deserialize(bytes, &pool, dst.CreateSpec()), IsError());
}

TEST(ArrowDeserializerTest, RejectsGarbageInput) {
  StringPool pool;
  std::vector<uint8_t> bytes(256, 0xCD);
  auto dst = Dataframe::CreateFromTypedSpec(kUint32NonNull, &pool);
  EXPECT_THAT(Deserialize(bytes, &pool, dst.CreateSpec()), IsError());
}

}  // namespace
}  // namespace perfetto::trace_processor::core::dataframe
