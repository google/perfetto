/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/importers/perf/perf_data_reader.h"

#include <stddef.h>

#include "perfetto/base/build_config.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace perf_importer {

namespace {
template <typename T>
TraceBlobView TraceBlobViewFromVector(std::vector<T> nums) {
  size_t data_size = sizeof(T) * nums.size();
  auto blob = TraceBlob::Allocate(data_size);
  memcpy(blob.data(), nums.data(), data_size);
  return TraceBlobView(std::move(blob));
}
}  // namespace

TEST(PerfDataReaderUnittest, AppendToEmpty) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{1, 2, 3});
  Reader reader;
  EXPECT_FALSE(reader.CanReadSize(1));
  reader.Append(std::move(tbv));
  EXPECT_TRUE(reader.CanReadSize(sizeof(uint64_t) * 2));
}

TEST(PerfDataReaderUnittest, Append) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{1, 2, 3});
  Reader reader(std::move(tbv));

  EXPECT_TRUE(reader.CanReadSize(sizeof(uint64_t) * 3));
  EXPECT_FALSE(reader.CanReadSize(sizeof(uint64_t) * 3 + 1));

  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 2}));
  EXPECT_TRUE(reader.CanReadSize(sizeof(uint64_t) * 5));
}

TEST(PerfDataReaderUnittest, Read) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));
  uint64_t val;
  reader.Read(val);
  EXPECT_EQ(val, 2u);
}

TEST(PerfDataReaderUnittest, ReadFromBuffer) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 6});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3}));

  // Now the first vector should be in the buffer.
  uint64_t val;
  reader.Read(val);
  EXPECT_EQ(val, 2u);
}

TEST(PerfDataReaderUnittest, ReadBetweenBufferAndBlob) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3, 5}));

  struct Nums {
    uint64_t x;
    uint64_t y;
    uint64_t z;
  };

  Nums nums;
  reader.Read(nums);

  EXPECT_EQ(nums.x, 2u);
  EXPECT_EQ(nums.y, 4u);
  EXPECT_EQ(nums.z, 1u);
}

TEST(PerfDataReaderUnittest, ReadOptional) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));
  std::optional<uint64_t> val;
  reader.ReadOptional(val);
  EXPECT_EQ(val, 2u);
}

TEST(PerfDataReaderUnittest, ReadVector) {
  TraceBlobView tbv =
      TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8, 16, 32});
  Reader reader(std::move(tbv));

  std::vector<uint64_t> res(3);
  reader.ReadVector(res);

  std::vector<uint64_t> valid{2, 4, 8};
  EXPECT_EQ(res, valid);
}

TEST(PerfDataReaderUnittest, Skip) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));

  reader.Skip<uint64_t>();

  uint64_t val;
  reader.Read(val);
  EXPECT_EQ(val, 4u);
}

TEST(PerfDataReaderUnittest, SkipInBuffer) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3, 5}));

  reader.Skip<uint64_t>();
  EXPECT_EQ(reader.current_file_offset(), sizeof(uint64_t));
}

TEST(PerfDataReaderUnittest, SkipBetweenBufferAndBlob) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3, 5}));

  struct Nums {
    uint64_t x;
    uint64_t y;
    uint64_t z;
  };

  reader.Skip<Nums>();
  EXPECT_EQ(reader.current_file_offset(), sizeof(Nums));
}

TEST(PerfDataReaderUnittest, Peek) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));

  uint64_t peek_val;
  reader.Peek(peek_val);

  uint64_t val;
  reader.Read(val);
  EXPECT_EQ(val, 2u);
}

TEST(PerfDataReaderUnittest, PeekFromBuffer) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 6});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3}));

  uint64_t val;
  reader.Peek(val);
  EXPECT_EQ(val, 2u);
}

TEST(PerfDataReaderUnittest, PeekBetweenBufferAndBlob) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3, 5}));

  struct Nums {
    uint64_t x;
    uint64_t y;
    uint64_t z;
  };

  Nums nums;
  reader.Peek(nums);

  EXPECT_EQ(nums.x, 2u);
  EXPECT_EQ(nums.y, 4u);
  EXPECT_EQ(nums.z, 1u);
}

TEST(PerfDataReaderUnittest, GetTraceBlobView) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));
  EXPECT_TRUE(reader.CanReadSize(sizeof(uint64_t) * 3));

  TraceBlobView new_tbv = reader.PeekTraceBlobView(sizeof(uint64_t) * 2);
  Reader new_reader(std::move(new_tbv));
  EXPECT_TRUE(new_reader.CanReadSize(sizeof(uint64_t) * 2));
  EXPECT_FALSE(new_reader.CanReadSize(sizeof(uint64_t) * 3));
}

TEST(PerfDataReaderUnittest, GetTraceBlobViewFromBuffer) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3, 5}));

  TraceBlobView new_tbv = reader.PeekTraceBlobView(sizeof(uint64_t) * 2);
  Reader new_reader(std::move(new_tbv));
  EXPECT_TRUE(new_reader.CanReadSize(sizeof(uint64_t) * 2));
  EXPECT_FALSE(new_reader.CanReadSize(sizeof(uint64_t) * 3));
}

TEST(PerfDataReaderUnittest, GetTraceBlobViewFromBetweenBufferAndBlob) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4});
  Reader reader(std::move(tbv));
  reader.Append(TraceBlobViewFromVector(std::vector<uint64_t>{1, 3, 5}));

  TraceBlobView new_tbv = reader.PeekTraceBlobView(sizeof(uint64_t) * 3);
  Reader new_reader(std::move(new_tbv));
  EXPECT_TRUE(new_reader.CanReadSize(sizeof(uint64_t) * 3));
  EXPECT_FALSE(new_reader.CanReadSize(sizeof(uint64_t) * 4));
}

TEST(PerfDataReaderUnittest, CanAccessFileRange) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));
  EXPECT_TRUE(reader.CanAccessFileRange(2, sizeof(uint64_t) * 3));
  EXPECT_FALSE(reader.CanAccessFileRange(2, sizeof(uint64_t) * 3 + 10));
}

}  // namespace perf_importer

}  // namespace trace_processor
}  // namespace perfetto
