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

#include "src/trace_processor/perfetto_sql/pipeline/plan_key.h"

#include <cstdint>
#include <string>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::pipeline {
namespace {

// The key 00 01 .. 0f of the SipHash paper's reference vectors.
constexpr uint64_t kK0 = 0x0706050403020100ull;
constexpr uint64_t kK1 = 0x0f0e0d0c0b0a0908ull;

std::string Counting(size_t size) {
  std::string data;
  for (size_t i = 0; i < size; ++i) {
    data.push_back(static_cast<char>(i));
  }
  return data;
}

TEST(PlanKeyTest, SipHashMatchesReferenceVectors) {
  EXPECT_EQ(PlanKey::SipHash(kK0, kK1, ""), 0x726fdb47dd0e0e31ull);
  EXPECT_EQ(PlanKey::SipHash(kK0, kK1, Counting(15)), 0xa129ca6149be45e5ull);
}

TEST(PlanKeyTest, StampsDependOnUseAndKey) {
  PlanKey key(kK0, kK1);
  EXPECT_NE(key.Stamp(PlanKey::Use::kPlan, "x"),
            key.Stamp(PlanKey::Use::kTable, "x"));
  EXPECT_NE(key.Stamp(PlanKey::Use::kPlan, "x"),
            PlanKey(kK0, kK1 + 1).Stamp(PlanKey::Use::kPlan, "x"));
}

}  // namespace
}  // namespace perfetto::trace_processor::pipeline
