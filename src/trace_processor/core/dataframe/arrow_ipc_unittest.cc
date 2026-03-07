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

#include "src/trace_processor/core/dataframe/arrow_ipc.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/util/trace_blob_view_reader.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

// Helper: serialize a Dataframe to Arrow IPC bytes using ArrowWriter.
base::Status SerializeToBytes(const Dataframe& df,
                              StringPool* pool,
                              std::vector<uint8_t>& out) {
  ArrowWriter writer;
  writer.Prepare(df, pool);
  return writer.Write(df, pool, [&](const uint8_t* data, size_t len) {
    out.insert(out.end(), data, data + len);
  });
}

inline constexpr auto kUint32Spec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Uint32{}, NonNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripUint32NonNullable) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kUint32Spec, &pool);

  src.InsertUnchecked(kUint32Spec, std::monostate{}, uint32_t{10});
  src.InsertUnchecked(kUint32Spec, std::monostate{}, uint32_t{20});
  src.InsertUnchecked(kUint32Spec, std::monostate{}, uint32_t{30});

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_FALSE(bytes.empty());

  // Check Arrow IPC file magic.
  ASSERT_GE(bytes.size(), 8u);
  EXPECT_EQ(memcmp(bytes.data(), "ARROW1", 6), 0);

  // Build a TraceBlobViewReader from the serialized bytes.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  // Deserialize into a fresh dataframe with the same schema.
  auto dst = Dataframe::CreateFromTypedSpec(kUint32Spec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);

  // Verify values.
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32Spec, 0)), uint32_t{10});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32Spec, 1)), uint32_t{20});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32Spec, 2)), uint32_t{30});
}

inline constexpr auto kUint32DenseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Uint32{}, DenseNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripUint32DenseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kUint32DenseNullSpec, &pool);

  src.InsertUnchecked(kUint32DenseNullSpec, std::monostate{},
                      std::optional<uint32_t>{10});
  src.InsertUnchecked(kUint32DenseNullSpec, std::monostate{},
                      std::optional<uint32_t>{});
  src.InsertUnchecked(kUint32DenseNullSpec, std::monostate{},
                      std::optional<uint32_t>{30});
  src.InsertUnchecked(kUint32DenseNullSpec, std::monostate{},
                      std::optional<uint32_t>{});
  src.InsertUnchecked(kUint32DenseNullSpec, std::monostate{},
                      std::optional<uint32_t>{50});

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kUint32DenseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 5u);

  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNullSpec, 0)),
            std::optional<uint32_t>{10});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNullSpec, 1)),
            std::optional<uint32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNullSpec, 2)),
            std::optional<uint32_t>{30});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNullSpec, 3)),
            std::optional<uint32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32DenseNullSpec, 4)),
            std::optional<uint32_t>{50});
}

inline constexpr auto kUint32SparseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Uint32{},
                          SparseNullWithPopcountAlways{},
                          Unsorted{}));

TEST(ArrowIpcTest, RoundTripUint32SparseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kUint32SparseNullSpec, &pool);

  src.InsertUnchecked(kUint32SparseNullSpec, std::monostate{},
                      std::optional<uint32_t>{10});
  src.InsertUnchecked(kUint32SparseNullSpec, std::monostate{},
                      std::optional<uint32_t>{});
  src.InsertUnchecked(kUint32SparseNullSpec, std::monostate{},
                      std::optional<uint32_t>{30});
  src.InsertUnchecked(kUint32SparseNullSpec, std::monostate{},
                      std::optional<uint32_t>{});
  src.InsertUnchecked(kUint32SparseNullSpec, std::monostate{},
                      std::optional<uint32_t>{50});

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kUint32SparseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 5u);

  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32SparseNullSpec, 0)),
            std::optional<uint32_t>{10});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32SparseNullSpec, 1)),
            std::optional<uint32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32SparseNullSpec, 2)),
            std::optional<uint32_t>{30});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32SparseNullSpec, 3)),
            std::optional<uint32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kUint32SparseNullSpec, 4)),
            std::optional<uint32_t>{50});
}

inline constexpr auto kStringNonNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "name"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripStringNonNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kStringNonNullSpec, &pool);

  auto hello = pool.InternString(base::StringView("hello"));
  auto world = pool.InternString(base::StringView("world"));
  auto empty = pool.InternString(base::StringView(""));

  src.InsertUnchecked(kStringNonNullSpec, std::monostate{}, hello);
  src.InsertUnchecked(kStringNonNullSpec, std::monostate{}, world);
  src.InsertUnchecked(kStringNonNullSpec, std::monostate{}, empty);

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kStringNonNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);

  auto v0 = dst.GetCellUnchecked<1>(kStringNonNullSpec, 0);
  auto v1 = dst.GetCellUnchecked<1>(kStringNonNullSpec, 1);
  auto v2 = dst.GetCellUnchecked<1>(kStringNonNullSpec, 2);
  EXPECT_EQ(pool.Get(v0).ToStdString(), "hello");
  EXPECT_EQ(pool.Get(v1).ToStdString(), "world");
  EXPECT_EQ(pool.Get(v2).ToStdString(), "");
}

inline constexpr auto kStringDenseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "name"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{}, DenseNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripStringDenseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kStringDenseNullSpec, &pool);

  auto hello = pool.InternString(base::StringView("hello"));
  auto world = pool.InternString(base::StringView("world"));

  src.InsertUnchecked(kStringDenseNullSpec, std::monostate{},
                      std::optional<StringPool::Id>{hello});
  src.InsertUnchecked(kStringDenseNullSpec, std::monostate{},
                      std::optional<StringPool::Id>{});
  src.InsertUnchecked(kStringDenseNullSpec, std::monostate{},
                      std::optional<StringPool::Id>{world});

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kStringDenseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);

  auto v0 = dst.GetCellUnchecked<1>(kStringDenseNullSpec, 0);
  auto v1 = dst.GetCellUnchecked<1>(kStringDenseNullSpec, 1);
  auto v2 = dst.GetCellUnchecked<1>(kStringDenseNullSpec, 2);
  ASSERT_TRUE(v0.has_value());
  EXPECT_EQ(pool.Get(*v0).ToStdString(), "hello");
  EXPECT_FALSE(v1.has_value());
  ASSERT_TRUE(v2.has_value());
  EXPECT_EQ(pool.Get(*v2).ToStdString(), "world");
}

inline constexpr auto kStringSparseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "name"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{},
                          SparseNullWithPopcountAlways{},
                          Unsorted{}));

TEST(ArrowIpcTest, RoundTripStringSparseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kStringSparseNullSpec, &pool);

  auto hello = pool.InternString(base::StringView("hello"));
  auto world = pool.InternString(base::StringView("world"));

  src.InsertUnchecked(kStringSparseNullSpec, std::monostate{},
                      std::optional<StringPool::Id>{hello});
  src.InsertUnchecked(kStringSparseNullSpec, std::monostate{},
                      std::optional<StringPool::Id>{});
  src.InsertUnchecked(kStringSparseNullSpec, std::monostate{},
                      std::optional<StringPool::Id>{world});

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kStringSparseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);

  auto v0 = dst.GetCellUnchecked<1>(kStringSparseNullSpec, 0);
  auto v1 = dst.GetCellUnchecked<1>(kStringSparseNullSpec, 1);
  auto v2 = dst.GetCellUnchecked<1>(kStringSparseNullSpec, 2);
  ASSERT_TRUE(v0.has_value());
  EXPECT_EQ(pool.Get(*v0).ToStdString(), "hello");
  EXPECT_FALSE(v1.has_value());
  ASSERT_TRUE(v2.has_value());
  EXPECT_EQ(pool.Get(*v2).ToStdString(), "world");
}

inline constexpr auto kInt32NonNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int32{}, NonNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripInt32NonNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt32NonNullSpec, &pool);

  src.InsertUnchecked(kInt32NonNullSpec, std::monostate{}, int32_t{-10});
  src.InsertUnchecked(kInt32NonNullSpec, std::monostate{}, int32_t{0});
  src.InsertUnchecked(kInt32NonNullSpec, std::monostate{}, int32_t{42});

  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kInt32NonNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32NonNullSpec, 0)), int32_t{-10});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32NonNullSpec, 1)), int32_t{0});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32NonNullSpec, 2)), int32_t{42});
}

inline constexpr auto kInt32DenseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int32{}, DenseNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripInt32DenseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt32DenseNullSpec, &pool);

  src.InsertUnchecked(kInt32DenseNullSpec, std::monostate{},
                      std::optional<int32_t>{-5});
  src.InsertUnchecked(kInt32DenseNullSpec, std::monostate{},
                      std::optional<int32_t>{});
  src.InsertUnchecked(kInt32DenseNullSpec, std::monostate{},
                      std::optional<int32_t>{99});

  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kInt32DenseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32DenseNullSpec, 0)),
            std::optional<int32_t>{-5});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32DenseNullSpec, 1)),
            std::optional<int32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32DenseNullSpec, 2)),
            std::optional<int32_t>{99});
}

inline constexpr auto kInt32SparseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int32{}, SparseNullWithPopcountAlways{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripInt32SparseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt32SparseNullSpec, &pool);

  src.InsertUnchecked(kInt32SparseNullSpec, std::monostate{},
                      std::optional<int32_t>{-5});
  src.InsertUnchecked(kInt32SparseNullSpec, std::monostate{},
                      std::optional<int32_t>{});
  src.InsertUnchecked(kInt32SparseNullSpec, std::monostate{},
                      std::optional<int32_t>{99});

  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kInt32SparseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32SparseNullSpec, 0)),
            std::optional<int32_t>{-5});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32SparseNullSpec, 1)),
            std::optional<int32_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt32SparseNullSpec, 2)),
            std::optional<int32_t>{99});
}

inline constexpr auto kInt64NonNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripInt64NonNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt64NonNullSpec, &pool);

  src.InsertUnchecked(kInt64NonNullSpec, std::monostate{},
                      int64_t{-1000000000000LL});
  src.InsertUnchecked(kInt64NonNullSpec, std::monostate{}, int64_t{0});
  src.InsertUnchecked(kInt64NonNullSpec, std::monostate{},
                      int64_t{1000000000000LL});

  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kInt64NonNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64NonNullSpec, 0)),
            int64_t{-1000000000000LL});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64NonNullSpec, 1)), int64_t{0});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64NonNullSpec, 2)),
            int64_t{1000000000000LL});
}

inline constexpr auto kInt64DenseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int64{}, DenseNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripInt64DenseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt64DenseNullSpec, &pool);

  src.InsertUnchecked(kInt64DenseNullSpec, std::monostate{},
                      std::optional<int64_t>{-100});
  src.InsertUnchecked(kInt64DenseNullSpec, std::monostate{},
                      std::optional<int64_t>{});
  src.InsertUnchecked(kInt64DenseNullSpec, std::monostate{},
                      std::optional<int64_t>{200});

  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kInt64DenseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64DenseNullSpec, 0)),
            std::optional<int64_t>{-100});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64DenseNullSpec, 1)),
            std::optional<int64_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64DenseNullSpec, 2)),
            std::optional<int64_t>{200});
}

inline constexpr auto kInt64SparseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int64{}, SparseNullWithPopcountAlways{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripInt64SparseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kInt64SparseNullSpec, &pool);

  src.InsertUnchecked(kInt64SparseNullSpec, std::monostate{},
                      std::optional<int64_t>{-100});
  src.InsertUnchecked(kInt64SparseNullSpec, std::monostate{},
                      std::optional<int64_t>{});
  src.InsertUnchecked(kInt64SparseNullSpec, std::monostate{},
                      std::optional<int64_t>{200});

  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kInt64SparseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64SparseNullSpec, 0)),
            std::optional<int64_t>{-100});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64SparseNullSpec, 1)),
            std::optional<int64_t>{});
  EXPECT_EQ((dst.GetCellUnchecked<1>(kInt64SparseNullSpec, 2)),
            std::optional<int64_t>{200});
}

inline constexpr auto kDoubleNonNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Double{}, NonNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripDoubleNonNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kDoubleNonNullSpec, &pool);

  src.InsertUnchecked(kDoubleNonNullSpec, std::monostate{}, 1.5);
  src.InsertUnchecked(kDoubleNonNullSpec, std::monostate{}, 2.75);
  src.InsertUnchecked(kDoubleNonNullSpec, std::monostate{}, 3.0);

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kDoubleNonNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);
  EXPECT_DOUBLE_EQ((dst.GetCellUnchecked<1>(kDoubleNonNullSpec, 0)), 1.5);
  EXPECT_DOUBLE_EQ((dst.GetCellUnchecked<1>(kDoubleNonNullSpec, 1)), 2.75);
  EXPECT_DOUBLE_EQ((dst.GetCellUnchecked<1>(kDoubleNonNullSpec, 2)), 3.0);
}

inline constexpr auto kDoubleDenseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Double{}, DenseNull{}, Unsorted{}));

TEST(ArrowIpcTest, RoundTripDoubleDenseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kDoubleDenseNullSpec, &pool);

  src.InsertUnchecked(kDoubleDenseNullSpec, std::monostate{},
                      std::optional<double>{1.5});
  src.InsertUnchecked(kDoubleDenseNullSpec, std::monostate{},
                      std::optional<double>{});
  src.InsertUnchecked(kDoubleDenseNullSpec, std::monostate{},
                      std::optional<double>{3.0});

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kDoubleDenseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);

  auto v0 = dst.GetCellUnchecked<1>(kDoubleDenseNullSpec, 0);
  auto v1 = dst.GetCellUnchecked<1>(kDoubleDenseNullSpec, 1);
  auto v2 = dst.GetCellUnchecked<1>(kDoubleDenseNullSpec, 2);
  ASSERT_TRUE(v0.has_value());
  EXPECT_DOUBLE_EQ(*v0, 1.5);
  EXPECT_FALSE(v1.has_value());
  ASSERT_TRUE(v2.has_value());
  EXPECT_DOUBLE_EQ(*v2, 3.0);
}

inline constexpr auto kDoubleSparseNullSpec = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Double{},
                          SparseNullWithPopcountAlways{},
                          Unsorted{}));

TEST(ArrowIpcTest, RoundTripDoubleSparseNull) {
  StringPool pool;
  auto src = Dataframe::CreateFromTypedSpec(kDoubleSparseNullSpec, &pool);

  src.InsertUnchecked(kDoubleSparseNullSpec, std::monostate{},
                      std::optional<double>{1.5});
  src.InsertUnchecked(kDoubleSparseNullSpec, std::monostate{},
                      std::optional<double>{});
  src.InsertUnchecked(kDoubleSparseNullSpec, std::monostate{},
                      std::optional<double>{3.0});

  // Serialize.
  std::vector<uint8_t> bytes;
  auto status = SerializeToBytes(src, &pool, bytes);
  ASSERT_TRUE(status.ok()) << status.message();

  // Deserialize.
  util::TraceBlobViewReader reader;
  reader.PushBack(
      TraceBlobView(TraceBlob::CopyFrom(bytes.data(), bytes.size())));

  auto dst = Dataframe::CreateFromTypedSpec(kDoubleSparseNullSpec, &pool);
  status = DeserializeFromArrowIpc(dst, &pool, reader);
  ASSERT_TRUE(status.ok()) << status.message();

  EXPECT_EQ(dst.row_count(), 3u);

  auto v0 = dst.GetCellUnchecked<1>(kDoubleSparseNullSpec, 0);
  auto v1 = dst.GetCellUnchecked<1>(kDoubleSparseNullSpec, 1);
  auto v2 = dst.GetCellUnchecked<1>(kDoubleSparseNullSpec, 2);
  ASSERT_TRUE(v0.has_value());
  EXPECT_DOUBLE_EQ(*v0, 1.5);
  EXPECT_FALSE(v1.has_value());
  ASSERT_TRUE(v2.has_value());
  EXPECT_DOUBLE_EQ(*v2, 3.0);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::dataframe
