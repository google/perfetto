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

#include "src/trace_processor/core/dataframe/arrow_serializer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/arrow_internal.h"
#include "src/trace_processor/core/dataframe/arrow_test_utils.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/util/flatbuffer_reader.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {
namespace {

using arrow_internal::ArrowBuffer;
using arrow_internal::Block;
using arrow_internal::FieldNode;
using arrow_internal::Load;
using arrow_test::MakeDataframe;
using arrow_test::Serialize;
using perfetto::base::gtest_matchers::IsError;

inline constexpr auto kUint32NonNull = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Uint32{}, NonNull{}, Unsorted{}));

inline constexpr auto kStringNonNull = CreateTypedDataframeSpec(
    {"_auto_id", "str"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}));

inline constexpr auto kInt32Sparse = CreateTypedDataframeSpec(
    {"_auto_id", "val"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(Int32{}, SparseNull{}, Unsorted{}));

inline constexpr auto kIdOnly = CreateTypedDataframeSpec(
    {"_auto_id"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}));

struct SerializedRecordBatch {
  Block block;
  int64_t rows;
  size_t body_offset;
  util::FlatBufferScalarVec<FieldNode> nodes;
  util::FlatBufferScalarVec<ArrowBuffer> buffers;
};

std::optional<SerializedRecordBatch> ReadRecordBatch(
    const std::vector<uint8_t>& bytes) {
  using namespace arrow_internal;
  if (bytes.size() < kMinimumFileSize) {
    return std::nullopt;
  }
  uint32_t footer_size =
      Load<uint32_t>(bytes.data() + bytes.size() - kFileTrailerSize, 0);
  if (footer_size > bytes.size() - kFileTrailerSize) {
    return std::nullopt;
  }
  size_t footer_offset = bytes.size() - kFileTrailerSize - footer_size;
  auto footer = util::FlatBufferReader::GetRoot(bytes.data() + footer_offset,
                                                footer_size);
  if (!footer) {
    return std::nullopt;
  }
  auto blocks = footer->VecScalar<Block>(footer_field::kRecordBatches);
  if (blocks.size() != kRecordBatchCount) {
    return std::nullopt;
  }
  Block block = blocks[0];
  if (block.offset < 0 || block.metadata_length < 0 ||
      static_cast<uint64_t>(block.offset) +
              static_cast<uint32_t>(block.metadata_length) >
          bytes.size()) {
    return std::nullopt;
  }
  size_t message_offset = static_cast<size_t>(block.offset);
  MessagePrefix prefix = Load<MessagePrefix>(bytes.data() + message_offset, 0);
  if (prefix.continuation != kContinuation || prefix.metadata_size <= 0) {
    return std::nullopt;
  }
  auto message = util::FlatBufferReader::GetRoot(
      bytes.data() + message_offset + kMessagePrefixSize,
      static_cast<uint32_t>(prefix.metadata_size));
  auto record_batch = message ? message->Table(message_field::kHeader)
                              : util::FlatBufferReader{};
  if (!record_batch) {
    return std::nullopt;
  }
  return SerializedRecordBatch{
      block, record_batch.Scalar<int64_t>(record_batch_field::kLength),
      message_offset + static_cast<uint32_t>(block.metadata_length),
      record_batch.VecScalar<FieldNode>(record_batch_field::kNodes),
      record_batch.VecScalar<ArrowBuffer>(record_batch_field::kBuffers)};
}

template <typename T>
T ReadBodyValue(const std::vector<uint8_t>& bytes,
                const SerializedRecordBatch& batch,
                uint32_t buffer_index,
                uint32_t value_index) {
  ArrowBuffer buffer = batch.buffers[buffer_index];
  return Load<T>(
      bytes.data() + batch.body_offset + static_cast<size_t>(buffer.offset),
      value_index);
}

TEST(ArrowSerializerTest, WritesArrowFileMagic) {
  constexpr char kArrowMagic[] = "ARROW1";
  constexpr size_t kArrowMagicSize = sizeof(kArrowMagic) - 1;

  StringPool pool;
  auto source = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(source, pool);

  ASSERT_GE(bytes.size(), 2 * kArrowMagicSize);
  EXPECT_EQ(memcmp(bytes.data(), kArrowMagic, kArrowMagicSize), 0);
  EXPECT_EQ(memcmp(bytes.data() + bytes.size() - kArrowMagicSize, kArrowMagic,
                   kArrowMagicSize),
            0);
}

TEST(ArrowSerializerTest, WritesPrimitiveBuffer) {
  StringPool pool;
  auto source = MakeDataframe(kUint32NonNull, &pool, uint32_t{10}, uint32_t{20},
                              uint32_t{30});
  std::vector<uint8_t> bytes = Serialize(source, pool);
  auto batch = ReadRecordBatch(bytes);

  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->rows, 3);
  ASSERT_EQ(batch->nodes.size(), 1u);
  EXPECT_EQ(batch->nodes[0].null_count, 0);
  ASSERT_EQ(batch->buffers.size(), 2u);
  EXPECT_EQ(batch->buffers[0].length, 0);
  EXPECT_EQ(batch->buffers[1].length,
            static_cast<int64_t>(3 * sizeof(uint32_t)));
  EXPECT_EQ(ReadBodyValue<uint32_t>(bytes, *batch, 1, 0), 10u);
  EXPECT_EQ(ReadBodyValue<uint32_t>(bytes, *batch, 1, 1), 20u);
  EXPECT_EQ(ReadBodyValue<uint32_t>(bytes, *batch, 1, 2), 30u);
}

TEST(ArrowSerializerTest, WritesValidityAndDensifiesSparseNumericBuffer) {
  StringPool pool;
  auto source =
      MakeDataframe(kInt32Sparse, &pool, std::optional<int32_t>{11},
                    std::optional<int32_t>{}, std::optional<int32_t>{33});
  std::vector<uint8_t> bytes = Serialize(source, pool);
  auto batch = ReadRecordBatch(bytes);

  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->nodes.size(), 1u);
  EXPECT_EQ(batch->nodes[0].null_count, 1);
  ASSERT_EQ(batch->buffers.size(), 2u);
  EXPECT_EQ(batch->buffers[0].length, 1);
  EXPECT_EQ(ReadBodyValue<uint8_t>(bytes, *batch, 0, 0), 0b00000101);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 0), 11);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 1), 0);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 2), 33);
}

TEST(ArrowSerializerTest, WritesUtf8OffsetAndDataBuffers) {
  StringPool pool;
  auto hello = pool.InternString(base::StringView("hello"));
  auto empty = pool.InternString(base::StringView(""));
  auto world = pool.InternString(base::StringView("world"));
  auto source = MakeDataframe(kStringNonNull, &pool, hello, empty, world);
  std::vector<uint8_t> bytes = Serialize(source, pool);
  auto batch = ReadRecordBatch(bytes);

  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->buffers.size(), 3u);
  EXPECT_EQ(batch->buffers[0].length, 0);
  EXPECT_EQ(batch->buffers[1].length,
            static_cast<int64_t>(4 * sizeof(int32_t)));
  EXPECT_EQ(batch->buffers[2].length, 10);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 0), 0);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 1), 5);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 2), 5);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 3), 10);
  ArrowBuffer strings = batch->buffers[2];
  const char* string_data = reinterpret_cast<const char*>(
      bytes.data() + batch->body_offset + strings.offset);
  EXPECT_EQ(std::string_view(string_data, static_cast<size_t>(strings.length)),
            "helloworld");
}

TEST(ArrowSerializerTest, OmitsImplicitIdColumn) {
  StringPool pool;
  auto source = Dataframe::CreateFromTypedSpec(kIdOnly, &pool);
  source.InsertUnchecked(kIdOnly, std::monostate{});
  source.InsertUnchecked(kIdOnly, std::monostate{});
  source.InsertUnchecked(kIdOnly, std::monostate{});
  std::vector<uint8_t> bytes = Serialize(source, pool);
  auto batch = ReadRecordBatch(bytes);

  ASSERT_TRUE(batch);
  EXPECT_EQ(batch->rows, 3);
  EXPECT_EQ(batch->nodes.size(), 0u);
  EXPECT_EQ(batch->buffers.size(), 0u);
  EXPECT_EQ(batch->block.body_length, 0);
}

TEST(ArrowSerializerTest, ValidatesPrepareWriteLifecycle) {
  StringPool pool;
  auto value = pool.InternString(base::StringView("value"));
  auto source = MakeDataframe(kStringNonNull, &pool, value);
  auto other = MakeDataframe(kStringNonNull, &pool, value);
  StringPool other_pool;
  ArrowSerializer serializer;
  auto sink = [](const uint8_t*, size_t) { return base::OkStatus(); };

  EXPECT_THAT(serializer.Write(source, pool, sink), IsError());
  EXPECT_THAT(serializer.Prepare(source, other_pool), IsError());
  ASSERT_OK(serializer.Prepare(source, pool));
  EXPECT_THAT(serializer.Write(other, pool, sink), IsError());
  EXPECT_THAT(serializer.Write(source, other_pool, sink), IsError());

  source.InsertUnchecked(kStringNonNull, std::monostate{}, value);
  EXPECT_THAT(serializer.Write(source, pool, sink), IsError());
}

TEST(ArrowSerializerTest, CanBeReused) {
  StringPool pool;
  auto first = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  auto second = MakeDataframe(kUint32NonNull, &pool, uint32_t{2}, uint32_t{3});
  ArrowSerializer serializer;
  std::vector<uint8_t> first_bytes;
  std::vector<uint8_t> second_bytes;

  auto write_to = [&](const Dataframe& dataframe, std::vector<uint8_t>* out) {
    auto size = serializer.Prepare(dataframe, pool);
    ASSERT_OK(size);
    ASSERT_OK(serializer.Write(dataframe, pool,
                               [&](const uint8_t* data, size_t length) {
                                 out->insert(out->end(), data, data + length);
                                 return base::OkStatus();
                               }));
    EXPECT_EQ(out->size(), *size);
  };
  write_to(first, &first_bytes);
  write_to(second, &second_bytes);

  EXPECT_EQ(first_bytes, Serialize(first, pool));
  EXPECT_EQ(second_bytes, Serialize(second, pool));
}

TEST(ArrowSerializerTest, PropagatesWriteError) {
  StringPool pool;
  auto source = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});

  ArrowSerializer serializer;
  ASSERT_OK(serializer.Prepare(source, pool));
  uint32_t calls = 0;
  base::Status status =
      serializer.Write(source, pool, [&](const uint8_t*, size_t) {
        if (++calls == 2) {
          return base::ErrStatus("sink failed");
        }
        return base::OkStatus();
      });
  EXPECT_THAT(status, IsError());
  EXPECT_EQ(status.message(), "sink failed");
  EXPECT_EQ(calls, 2u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::dataframe
