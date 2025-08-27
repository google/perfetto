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

#include "src/trace_processor/util/hyper_log_log.h"

#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace util {
namespace {

TEST(HyperLogLogTest, Empty) {
  HyperLogLog hll;
  ASSERT_NEAR(hll.Estimate(), 0, 0.01);
}

TEST(HyperLogLogTest, Single) {
  HyperLogLog hll;
  hll.Add(1);
  ASSERT_NEAR(hll.Estimate(), 1, 0.1);
}

TEST(HyperLogLogTest, Distinct) {
  HyperLogLog hll;
  for (int i = 0; i < 10000; ++i) {
    hll.Add(i);
  }
  ASSERT_NEAR(hll.Estimate(), 10000, 10000 * 0.1);
}

TEST(HyperLogLogTest, Repeated) {
  HyperLogLog hll;
  for (int i = 0; i < 10000; ++i) {
    hll.Add(i % 100);
  }
  ASSERT_NEAR(hll.Estimate(), 100, 100 * 0.1);
}

TEST(HyperLogLogTest, String) {
  HyperLogLog hll;
  hll.Add(std::string("hello"));
  hll.Add(std::string("world"));
  ASSERT_NEAR(hll.Estimate(), 2, 0.2);
}

TEST(HyperLogLogTest, Double) {
  HyperLogLog hll;
  hll.Add(1.23);
  hll.Add(4.56);
  ASSERT_NEAR(hll.Estimate(), 2, 0.2);
}

}  // namespace
}  // namespace util
}  // namespace trace_processor
}  // namespace perfetto
