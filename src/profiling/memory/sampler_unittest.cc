/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/profiling/memory/sampler.h"

#include "gtest/gtest.h"

#include <thread>

#include "src/profiling/memory/client.h"  // For PThreadKey.

namespace perfetto {
namespace {

TEST(SamplerTest, TestLarge) {
  PThreadKey key(ThreadLocalSamplingData::KeyDestructor);
  ASSERT_TRUE(key.valid());
  EXPECT_EQ(ShouldSample(key.get(), 1024, 512, malloc, free), 1);
}

TEST(SamplerTest, TestSmall) {
  PThreadKey key(ThreadLocalSamplingData::KeyDestructor);
  ASSERT_TRUE(key.valid());
  // As we initialize interval_to_next_sample_ with 0, the first sample
  // should always get sampled.
  EXPECT_EQ(ShouldSample(key.get(), 1, 512, malloc, free), 1);
}

TEST(SamplerTest, TestSmallFromThread) {
  PThreadKey key(ThreadLocalSamplingData::KeyDestructor);
  ASSERT_TRUE(key.valid());
  std::thread th([&key] {
    // As we initialize interval_to_next_sample_ with 0, the first sample
    // should always get sampled.
    EXPECT_EQ(ShouldSample(key.get(), 1, 512, malloc, free), 1);
  });
  std::thread th2([&key] {
    // The threads should have separate state.
    EXPECT_EQ(ShouldSample(key.get(), 1, 512, malloc, free), 1);
  });
  th.join();
  th2.join();
}

}  // namespace
}  // namespace perfetto
