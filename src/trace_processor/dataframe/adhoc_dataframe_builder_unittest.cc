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

#include "src/trace_processor/dataframe/adhoc_dataframe_builder.h"

#include <cstdint>

#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::dataframe {
namespace {

using ::testing::ElementsAre;
using SmallValueEq = impl::SpecializedStorage::SmallValueEq;
using SmallValueEqNoDup = impl::SpecializedStorage::SmallValueEqNoDup;
using SmallValueEqSortedNoDup =
    impl::SpecializedStorage::SmallValueEqSortedNoDup;

TEST(AdhocDataframeBuilder, BuildSmallValueEqSortedNoDup) {
  impl::FlexVector<uint32_t> data;
  data.push_back(0);
  data.push_back(1);
  data.push_back(3);
  data.push_back(4);

  AdhocDataframeBuilder::IntegerColumnSummary summary;
  summary.max = 4;

  auto storage = AdhocDataframeBuilder::BuildSmallValueEqSortedNoDupForTesting(
      data, summary);
  const auto& spec = storage.unchecked_get<SmallValueEqSortedNoDup>();

  ASSERT_TRUE(spec.bit_vector.is_set(0));
  ASSERT_TRUE(spec.bit_vector.is_set(1));
  ASSERT_FALSE(spec.bit_vector.is_set(2));
  ASSERT_TRUE(spec.bit_vector.is_set(3));
  ASSERT_TRUE(spec.bit_vector.is_set(4));

  // Popcount is not computed by this function.
}

TEST(AdhocDataframeBuilder, BuildSmallValueEqNoDup) {
  impl::FlexVector<uint32_t> data;
  data.push_back(3);
  data.push_back(1);
  data.push_back(4);
  data.push_back(0);

  AdhocDataframeBuilder::IntegerColumnSummary summary;
  summary.max = 4;

  auto storage =
      AdhocDataframeBuilder::BuildSmallValueEqNoDupForTesting(data, summary);
  const auto& spec = storage.unchecked_get<SmallValueEqNoDup>();

  EXPECT_THAT(spec.value_to_index, ElementsAre(3u, 1u, 0xffffffff, 0u, 2u));
}

TEST(AdhocDataframeBuilder, BuildSmallValueEq) {
  impl::FlexVector<uint32_t> data;
  data.push_back(0);
  data.push_back(1);
  data.push_back(0);
  data.push_back(2);
  data.push_back(1);

  AdhocDataframeBuilder::IntegerColumnSummary summary;
  summary.max = 2;

  auto storage =
      AdhocDataframeBuilder::BuildSmallValueEqForTesting(data, summary);
  const auto& spec = storage.unchecked_get<SmallValueEq>();

  EXPECT_THAT(spec.value_to_indices_start, ElementsAre(0u, 2u, 4u, 5u));
  EXPECT_THAT(spec.indices, ElementsAre(0u, 2u, 1u, 4u, 3u));
}

}  // namespace
}  // namespace perfetto::trace_processor::dataframe
