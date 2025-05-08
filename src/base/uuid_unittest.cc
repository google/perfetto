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

#include "perfetto/ext/base/uuid.h"

#include <array>
#include <cinttypes>
#include <set>

#include "perfetto/base/logging.h"
#include "perfetto/base/time.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(UuidTest, DefaultConstructorIsBlank) {
  Uuid a;
  Uuid b;
  EXPECT_EQ(a, b);
  EXPECT_EQ(a.msb(), 0);
  EXPECT_EQ(a.lsb(), 0);
}

TEST(UuidTest, TwoUuidsShouldBeDifferent) {
  Uuid a = Uuidv4();
  Uuid b = Uuidv4();
  EXPECT_NE(a, b);
  EXPECT_EQ(a, a);
  EXPECT_EQ(b, b);
}

TEST(UuidTest, CanRoundTripUuid) {
  Uuid uuid = Uuidv4();
  EXPECT_EQ(Uuid(uuid.ToString()), uuid);
}

TEST(UuidTest, SetGet) {
  Uuid a = Uuidv4();
  Uuid b;
  b.set_lsb_msb(a.lsb(), a.msb());
  EXPECT_EQ(a, b);
}

TEST(UuidTest, LsbMsbConstructor) {
  Uuid uuid(-6605018796207623390, 1314564453825188563);
  EXPECT_EQ(uuid.ToPrettyString(), "123e4567-e89b-12d3-a456-426655443322");
}

TEST(UuidTest, UuidToPrettyString) {
  Uuid uuid;
  uuid.set_lsb_msb(-6605018796207623390, 1314564453825188563);
  EXPECT_EQ(uuid.ToPrettyString(), "123e4567-e89b-12d3-a456-426655443322");
}

TEST(UuidTest, BoolOperator) {
  Uuid uuid;
  EXPECT_FALSE(uuid);

  uuid.set_lsb(1);
  EXPECT_TRUE(uuid);

  uuid.set_lsb(0);
  EXPECT_FALSE(uuid);

  uuid.set_msb(0x80000000);
  EXPECT_TRUE(uuid);

  uuid = Uuid();
  EXPECT_FALSE(uuid);

  uuid = Uuidv4();
  EXPECT_TRUE(uuid);
}

// Generate kRounds UUIDs and check that, for each bit, we see roughly as many
// zeros as ones.
// Marking as DISABLED as this really checks the STD implementation not our
// code. Invoke manually only when needed.
TEST(UuidTest, DISABLED_BitRandomDistribution) {
  const int kRounds = 100000;
  std::array<int64_t, 128> bit_count{};
  for (int i = 0; i < kRounds; i++) {
    Uuid uuid = Uuidv4();
    for (size_t b = 0; b < 64; b++) {
      bit_count[b] += (uint64_t(uuid.lsb()) & (1ull << b)) ? 1 : -1;
      bit_count[64 + b] += (uint64_t(uuid.msb()) & (1ull << b)) ? 1 : -1;
    }
  }

  // By adding +1 / -1 for each one/zero, `bit_count` contains for each bit,
  // their imbalance. In an ideal world we expect `bit_count` to be 0 at each
  // position. In practice we accept a 2% imbalance to pass the test.
  int64_t max_diff = 0;
  for (size_t i = 0; i < bit_count.size(); i++)
    max_diff = std::max(max_diff, std::abs(bit_count[i]));

  const double diff_pct =
      100.0 * static_cast<double>(max_diff) / static_cast<double>(kRounds);
  PERFETTO_DLOG("Max bit imbalance: %.2f %%", diff_pct);

  // Local runs show a 1% imbalance. We take a 5x margin for the test.
  ASSERT_LT(diff_pct, 5.0);
}

// This test checks for collisions in a space of 300M traces.
// It takes ~20  minutes to run (hence the disabled-by-default)
TEST(UuidTest, DISABLED_NoCollisions) {
  std::set<int64_t> rand_nums;
  uint64_t num_collisions = 0;
  const uint64_t kSpace = 300ull * 1000ull * 1000ull;
  const int64_t t_start = base::GetWallTimeMs().count();
  for (uint64_t i = 0; i < kSpace; i++) {
    Uuid uuid = Uuidv4();
    int64_t lsb = uuid.lsb();
    int64_t msb = uuid.msb();
    if (!rand_nums.insert(lsb).second || !rand_nums.insert(msb).second) {
      PERFETTO_ELOG("Found collision @ step %" PRIu64, i);
    }
    if (i % 1000000 == 0 && i > 0) {
      int64_t now = base::GetWallTimeMs().count();
      uint64_t elapsed = static_cast<uint64_t>(now - t_start);
      uint64_t eta_ms = kSpace * elapsed / i - elapsed;
      PERFETTO_LOG("Running... %" PRIu64 " %%, ETA: %" PRIu64 " seconds",
                   i * 100 / kSpace, eta_ms / 1000);
    }
  }
  EXPECT_EQ(num_collisions, 0u);
}

}  // namespace
}  // namespace base
}  // namespace perfetto
