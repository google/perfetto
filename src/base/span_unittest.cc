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

#include "perfetto/ext/base/span.h"

#include <array>
#include <cstdint>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {

TEST(SpanTest, CreatesFromVector) {
  std::vector<uint32_t> values{1, 2, 3};
  Span<const uint32_t> span = values;
  EXPECT_THAT(span, testing::ElementsAre(1u, 2u, 3u));
  EXPECT_THAT(span.subspan(1, 2), testing::ElementsAre(2u, 3u));

  Span<uint32_t> mutable_span = values;
  mutable_span.b[1] = 4;
  EXPECT_EQ(values[1], 4u);
}

TEST(SpanTest, CreatesFromArray) {
  std::array<uint32_t, 3> values{1, 2, 3};
  Span<const uint32_t> values_span = values;
  EXPECT_THAT(values_span, testing::ElementsAre(1u, 2u, 3u));

  Span<uint32_t> mutable_values = values;
  mutable_values[1] = 4;
  EXPECT_EQ(values[1], 4u);
}

}  // namespace
}  // namespace perfetto::base
