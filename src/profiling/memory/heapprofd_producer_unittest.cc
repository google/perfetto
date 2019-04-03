/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/memory/heapprofd_producer.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace perfetto {
namespace profiling {

using ::testing::Contains;
using ::testing::Pair;

TEST(LogHistogramTest, Simple) {
  LogHistogram h;
  h.Add(1);
  h.Add(0);
  EXPECT_THAT(h.GetData(), Contains(Pair(2, 1)));
  EXPECT_THAT(h.GetData(), Contains(Pair(1, 1)));
}

TEST(LogHistogramTest, Overflow) {
  LogHistogram h;
  h.Add(std::numeric_limits<uint64_t>::max());
  EXPECT_THAT(h.GetData(), Contains(Pair(LogHistogram::kMaxBucket, 1)));
}

}  // namespace profiling
}  // namespace perfetto
