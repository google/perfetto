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

#include "src/trace_processor/importers/perf/reader.h"

#include <stddef.h>
#include <cstdint>

#include "perfetto/trace_processor/trace_blob.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::perf_importer {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::SizeIs;

template <typename T>
TraceBlobView TraceBlobViewFromVector(std::vector<T> nums) {
  size_t data_size = sizeof(T) * nums.size();
  auto blob = TraceBlob::Allocate(data_size);
  memcpy(blob.data(), nums.data(), data_size);
  return TraceBlobView(std::move(blob));
}

TEST(ReaderUnittest, Read) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));
  uint64_t val;
  reader.Read(val);
  EXPECT_EQ(val, 2u);
}

TEST(ReaderUnittest, ReadOptional) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));
  std::optional<uint64_t> val;
  reader.ReadOptional(val);
  EXPECT_EQ(val, 2u);
}

TEST(ReaderUnittest, ReadVector) {
  TraceBlobView tbv =
      TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8, 16, 32});
  Reader reader(std::move(tbv));

  std::vector<uint64_t> res(3);
  reader.ReadVector(res);

  std::vector<uint64_t> valid{2, 4, 8};
  EXPECT_EQ(res, valid);
}

TEST(ReaderUnittest, Skip) {
  TraceBlobView tbv = TraceBlobViewFromVector(std::vector<uint64_t>{2, 4, 8});
  Reader reader(std::move(tbv));

  reader.Skip<uint64_t>();

  uint64_t val;
  reader.Read(val);
  EXPECT_EQ(val, 4u);
}

}  // namespace
}  // namespace perfetto::trace_processor::perf_importer
