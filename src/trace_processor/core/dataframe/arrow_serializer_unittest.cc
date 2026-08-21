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

inline constexpr auto kTwoStrings = CreateTypedDataframeSpec(
    {"_auto_id", "first", "second"},
    CreateTypedColumnSpec(Id{}, NonNull{}, IdSorted{}, NoDuplicates{}),
    CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}),
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

size_t FooterOffset(const std::vector<uint8_t>& bytes) {
  using namespace arrow_internal;
  uint32_t footer_size =
      Load<uint32_t>(bytes.data() + bytes.size() - kFileTrailerSize, 0);
  return bytes.size() - kFileTrailerSize - footer_size;
}

std::optional<SerializedRecordBatch> ReadBatch(
    const std::vector<uint8_t>& bytes,
    uint32_t footer_blocks_field,
    uint32_t index) {
  using namespace arrow_internal;
  if (bytes.size() < kMinimumFileSize) {
    return std::nullopt;
  }
  uint32_t footer_size =
      Load<uint32_t>(bytes.data() + bytes.size() - kFileTrailerSize, 0);
  if (footer_size > bytes.size() - kFileTrailerSize) {
    return std::nullopt;
  }
  size_t footer_offset = FooterOffset(bytes);
  auto footer = util::FlatBufferReader::GetRoot(bytes.data() + footer_offset,
                                                footer_size);
  if (!footer) {
    return std::nullopt;
  }
  auto blocks = footer->VecScalar<Block>(footer_blocks_field);
  if (index >= blocks.size()) {
    return std::nullopt;
  }
  Block block = blocks[index];
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
  auto header = message ? message->Table(message_field::kHeader)
                        : util::FlatBufferReader{};
  auto record_batch = footer_blocks_field == footer_field::kDictionaries
                          ? header.Table(dictionary_batch_field::kData)
                          : header;
  if (!record_batch) {
    return std::nullopt;
  }
  return SerializedRecordBatch{
      block, record_batch.Scalar<int64_t>(record_batch_field::kLength),
      message_offset + static_cast<uint32_t>(block.metadata_length),
      record_batch.VecScalar<FieldNode>(record_batch_field::kNodes),
      record_batch.VecScalar<ArrowBuffer>(record_batch_field::kBuffers)};
}

std::optional<SerializedRecordBatch> ReadRecordBatch(
    const std::vector<uint8_t>& bytes) {
  return ReadBatch(bytes, arrow_internal::footer_field::kRecordBatches, 0);
}

std::optional<SerializedRecordBatch> ReadDictionaryBatch(
    const std::vector<uint8_t>& bytes,
    uint32_t index) {
  return ReadBatch(bytes, arrow_internal::footer_field::kDictionaries, index);
}

// Reads the schema message, which is the first message in the file.
util::FlatBufferReader ReadSchema(const std::vector<uint8_t>& bytes) {
  using namespace arrow_internal;
  MessagePrefix prefix =
      Load<MessagePrefix>(bytes.data() + sizeof(kPaddedMagic), 0);
  EXPECT_EQ(prefix.continuation, kContinuation);
  auto message = util::FlatBufferReader::GetRoot(
      bytes.data() + sizeof(kPaddedMagic) + kMessagePrefixSize,
      static_cast<uint32_t>(prefix.metadata_size));
  EXPECT_TRUE(message.has_value());
  auto schema = message->Table(message_field::kHeader);
  EXPECT_TRUE(static_cast<bool>(schema));
  return schema;
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

TEST(ArrowSerializerTest, WritesDictionaryIndicesAndValues) {
  StringPool pool;
  auto hello = pool.InternString(base::StringView("hello"));
  auto empty = pool.InternString(base::StringView(""));
  auto world = pool.InternString(base::StringView("world"));
  auto source =
      MakeDataframe(kStringNonNull, &pool, hello, empty, world, hello);
  std::vector<uint8_t> bytes = Serialize(source, pool);
  auto batch = ReadRecordBatch(bytes);
  auto dictionary = ReadDictionaryBatch(bytes, 0);

  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->rows, 4);
  ASSERT_EQ(batch->buffers.size(), 2u);
  EXPECT_EQ(batch->buffers[0].length, 0);
  EXPECT_EQ(batch->buffers[1].length,
            static_cast<int64_t>(4 * sizeof(int32_t)));
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 0), 0);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 1), 1);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 2), 2);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *batch, 1, 3), 0);

  ASSERT_TRUE(dictionary);
  ASSERT_EQ(dictionary->rows, 3);
  ASSERT_EQ(dictionary->buffers.size(), 3u);
  EXPECT_EQ(dictionary->buffers[0].length, 0);
  EXPECT_EQ(dictionary->buffers[1].length,
            static_cast<int64_t>(4 * sizeof(int32_t)));
  EXPECT_EQ(dictionary->buffers[2].length, 10);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *dictionary, 1, 0), 0);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *dictionary, 1, 1), 5);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *dictionary, 1, 2), 5);
  EXPECT_EQ(ReadBodyValue<int32_t>(bytes, *dictionary, 1, 3), 10);
  ArrowBuffer strings = dictionary->buffers[2];
  const char* string_data = reinterpret_cast<const char*>(
      bytes.data() + dictionary->body_offset + strings.offset);
  EXPECT_EQ(std::string_view(string_data, static_cast<size_t>(strings.length)),
            "helloworld");
}

TEST(ArrowSerializerTest, WritesDictionaryEncodingInSchema) {
  using namespace arrow_internal;
  StringPool pool;
  auto hello = pool.InternString(base::StringView("hello"));
  auto source = MakeDataframe(kStringNonNull, &pool, hello);
  std::vector<uint8_t> bytes = Serialize(source, pool);

  auto fields = ReadSchema(bytes).VecTable(schema_field::kFields);
  ASSERT_EQ(fields.size(), 1u);
  EXPECT_EQ(fields[0].Scalar<uint8_t>(field_field::kTypeType), kTypeUtf8);
  auto encoding = fields[0].Table(field_field::kDictionary);
  ASSERT_TRUE(static_cast<bool>(encoding));
  EXPECT_EQ(encoding.Scalar<int64_t>(dictionary_encoding_field::kId,
                                     kMissingSignedValue),
            0);
  auto index_type = encoding.Table(dictionary_encoding_field::kIndexType);
  ASSERT_TRUE(static_cast<bool>(index_type));
  EXPECT_EQ(index_type.Scalar<int32_t>(int_field::kBitWidth),
            kDictionaryIndexBits);
  EXPECT_TRUE(index_type.Scalar<bool>(int_field::kIsSigned));
}

TEST(ArrowSerializerTest, WritesOneDictionaryPerStringColumn) {
  StringPool pool;
  auto a = pool.InternString(base::StringView("a"));
  auto source = Dataframe::CreateFromTypedSpec(kTwoStrings, &pool);
  source.InsertUnchecked(kTwoStrings, std::monostate{}, a, a);
  std::vector<uint8_t> bytes = Serialize(source, pool);

  ASSERT_TRUE(ReadDictionaryBatch(bytes, 0));
  ASSERT_TRUE(ReadDictionaryBatch(bytes, 1));
  EXPECT_FALSE(ReadDictionaryBatch(bytes, 2));
  auto batch = ReadRecordBatch(bytes);
  ASSERT_TRUE(batch);
  EXPECT_EQ(batch->buffers.size(), 2u * arrow_internal::kFixedWidthBufferCount);
}

TEST(ArrowSerializerTest, WritesEndOfStreamMarkerBeforeFooter) {
  using namespace arrow_internal;
  StringPool pool;
  auto source = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  std::vector<uint8_t> bytes = Serialize(source, pool);
  auto batch = ReadRecordBatch(bytes);

  ASSERT_TRUE(batch);
  size_t batch_end =
      batch->body_offset + static_cast<size_t>(batch->block.body_length);
  MessagePrefix marker = Load<MessagePrefix>(bytes.data() + batch_end, 0);
  EXPECT_EQ(marker.continuation, kContinuation);
  EXPECT_EQ(marker.metadata_size, 0);
  EXPECT_EQ(batch_end + kEndOfStreamSize, FooterOffset(bytes));
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

TEST(ArrowSerializerTest, MaterializesImplicitIdColumn) {
  StringPool pool;
  auto source = Dataframe::CreateFromTypedSpec(kIdOnly, &pool);
  source.InsertUnchecked(kIdOnly, std::monostate{});
  source.InsertUnchecked(kIdOnly, std::monostate{});
  source.InsertUnchecked(kIdOnly, std::monostate{});
  std::vector<uint8_t> bytes =
      Serialize(source, pool, ArrowSerializer::IdColumnMode::kInclude);
  auto batch = ReadRecordBatch(bytes);

  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->nodes.size(), 1u);
  ASSERT_EQ(batch->buffers.size(), 2u);
  EXPECT_EQ(batch->buffers[0].length, 0);
  EXPECT_EQ(batch->buffers[1].length,
            static_cast<int64_t>(3 * sizeof(uint32_t)));
  EXPECT_EQ(ReadBodyValue<uint32_t>(bytes, *batch, 1, 0), 0u);
  EXPECT_EQ(ReadBodyValue<uint32_t>(bytes, *batch, 1, 1), 1u);
  EXPECT_EQ(ReadBodyValue<uint32_t>(bytes, *batch, 1, 2), 2u);
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
  EXPECT_THAT(serializer.Prepare(source, pool), IsError());

  source.Finalize();
  other.Finalize();
  EXPECT_THAT(serializer.Prepare(source, other_pool), IsError());
  ASSERT_OK(serializer.Prepare(source, pool));
  EXPECT_THAT(serializer.Write(other, pool, sink), IsError());
  EXPECT_THAT(serializer.Write(source, other_pool, sink), IsError());
  EXPECT_OK(serializer.Write(source, pool, sink));
}

TEST(ArrowSerializerTest, CanBeReused) {
  StringPool pool;
  auto first = MakeDataframe(kUint32NonNull, &pool, uint32_t{1});
  auto second = MakeDataframe(kUint32NonNull, &pool, uint32_t{2}, uint32_t{3});
  first.Finalize();
  second.Finalize();
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

  source.Finalize();
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
