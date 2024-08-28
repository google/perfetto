/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/util/trace_blob_view_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <ostream>
#include <vector>

#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::util {
namespace {

using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::Optional;
using ::testing::Property;
using ::testing::SizeIs;

class SameDataAsMatcher {
 public:
  template <typename ArgType>
  class MatcherImpl : public ::testing ::MatcherInterface<const ArgType&> {
   public:
    explicit MatcherImpl(const TraceBlobView& expected_data)
        : expected_data_(expected_data) {}
    bool MatchAndExplain(const ArgType& arg,
                         ::testing ::MatchResultListener*) const override {
      return std::equal(expected_data_.data(),
                        expected_data_.data() + expected_data_.size(),
                        arg.data(), arg.data() + arg.size());
    }
    void DescribeTo(::std ::ostream*) const override {}
    void DescribeNegationTo(::std ::ostream*) const override {}

   private:
    const TraceBlobView& expected_data_;
  };

  explicit SameDataAsMatcher(const TraceBlobView& expected_data)
      : expected_data_(expected_data) {}

  template <typename ArgType>
  operator ::testing::Matcher<ArgType>() const {
    return ::testing::Matcher<ArgType>(
        new MatcherImpl<ArgType>(expected_data_));
  }

 private:
  const TraceBlobView& expected_data_;
};

SameDataAsMatcher SameDataAs(const TraceBlobView& expected_data) {
  return SameDataAsMatcher(expected_data);
}

TraceBlobView CreateExpectedData(size_t expected_size) {
  TraceBlob tb = TraceBlob::Allocate(expected_size);
  for (size_t i = 0; i < expected_size; ++i) {
    tb.data()[i] = static_cast<uint8_t>(i);
  }
  return TraceBlobView(std::move(tb));
}

std::vector<TraceBlobView> Slice(const TraceBlobView& blob, size_t chunk_size) {
  std::vector<TraceBlobView> chunks;
  size_t size = blob.size();
  for (size_t off = 0; size != 0;) {
    chunk_size = std::min(chunk_size, size);
    chunks.push_back(blob.slice_off(off, chunk_size));
    size -= chunk_size;
    off += chunk_size;
  }
  return chunks;
}

TraceBlobViewReader CreateTraceBlobViewReader(
    const std::vector<TraceBlobView>& chunks) {
  TraceBlobViewReader chunked_buffer;
  for (const auto& chunk : chunks) {
    chunked_buffer.PushBack(chunk.copy());
  }
  return chunked_buffer;
}

TEST(TraceBlobViewReader, ContiguousAccessAtOffset) {
  constexpr size_t kExpectedSize = 256;
  constexpr size_t kChunkSize = kExpectedSize / 4;
  TraceBlobView expected_data = CreateExpectedData(kExpectedSize);
  TraceBlobViewReader buffer =
      CreateTraceBlobViewReader(Slice(expected_data, kChunkSize));

  for (size_t file_offset = 0; file_offset <= kExpectedSize; ++file_offset) {
    EXPECT_TRUE(buffer.PopFrontUntil(file_offset));
    for (size_t off = file_offset; off <= kExpectedSize; ++off) {
      auto expected = expected_data.slice_off(off, kExpectedSize - off);
      std::optional<TraceBlobView> tbv = buffer.SliceOff(off, expected.size());
      EXPECT_THAT(tbv, Optional(SameDataAs(expected)));
    }
  }
}

TEST(TraceBlobViewReader, NoCopyIfDataIsContiguous) {
  constexpr size_t kExpectedSize = 256;
  constexpr size_t kChunkSize = kExpectedSize / 4;
  std::vector<TraceBlobView> chunks =
      Slice(CreateExpectedData(kExpectedSize), kChunkSize);
  TraceBlobViewReader buffer = CreateTraceBlobViewReader(chunks);

  for (size_t i = 0; i < chunks.size(); ++i) {
    for (size_t off = 0; off < kChunkSize; ++off) {
      const size_t expected_size = kChunkSize - off;
      EXPECT_THAT(
          buffer.SliceOff(i * kChunkSize + off, expected_size),
          Optional(Property(&TraceBlobView::data, Eq(chunks[i].data() + off))));
    }
  }
}

TEST(TraceBlobViewReader, PopRemovesData) {
  size_t expected_size = 256;
  size_t expected_file_offset = 0;
  const size_t kChunkSize = expected_size / 4;
  TraceBlobView expected_data = CreateExpectedData(expected_size);
  TraceBlobViewReader buffer =
      CreateTraceBlobViewReader(Slice(expected_data, kChunkSize));

  --expected_size;
  ++expected_file_offset;
  buffer.PopFrontUntil(expected_file_offset);
  EXPECT_THAT(buffer.start_offset(), Eq(expected_file_offset));
  EXPECT_THAT(buffer.SliceOff(expected_file_offset, expected_size),
              Optional(SameDataAs(expected_data.slice_off(
                  expected_data.size() - expected_size, expected_size))));

  expected_size -= kChunkSize;
  expected_file_offset += kChunkSize;
  buffer.PopFrontUntil(expected_file_offset);
  EXPECT_THAT(buffer.start_offset(), Eq(expected_file_offset));
  EXPECT_THAT(buffer.SliceOff(expected_file_offset, expected_size),
              Optional(SameDataAs(expected_data.slice_off(
                  expected_data.size() - expected_size, expected_size))));
}

}  // namespace
}  // namespace perfetto::trace_processor::util
