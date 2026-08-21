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

#include "src/trace_processor/core/util/span.h"

#include <cstdint>
#include <vector>

#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

TEST(SpanTest, CreatesFromVector) {
  std::vector<uint32_t> values{1, 2, 3};
  Span<const uint32_t> span = MakeSpan(values);
  EXPECT_THAT(span, testing::ElementsAre(1u, 2u, 3u));
  EXPECT_THAT(span.subspan(1, 2), testing::ElementsAre(2u, 3u));

  Span<uint32_t> mutable_span = MakeMutableSpan(values);
  mutable_span.b[1] = 4;
  EXPECT_EQ(values[1], 4u);
}

TEST(SpanTest, CreatesFromFixedSizeStorage) {
  auto flex = FlexVector<uint32_t>::CreateFilled(2, 3);
  EXPECT_THAT(flex.span(), testing::ElementsAre(3u, 3u));
  flex.mutable_span().b[0] = 4;
  EXPECT_EQ(flex[0], 4u);

  auto slab = Slab<uint32_t>::Alloc(2);
  slab[0] = 5;
  slab[1] = 5;
  Span<uint32_t> mutable_slab_span = slab;
  EXPECT_THAT(mutable_slab_span, testing::ElementsAre(5u, 5u));
  mutable_slab_span[1] = 6;
  const Slab<uint32_t>& const_slab = slab;
  Span<const uint32_t> slab_span = const_slab;
  EXPECT_EQ(slab_span[1], 6u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core
