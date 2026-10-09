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

#include "src/tracing/v2/producer_ring_buffer_config.h"

#include <stddef.h>
#include <stdint.h>

#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>

#include "perfetto/ext/base/utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::tracing_v2 {
namespace {

constexpr size_t kHeader = sizeof(RingBufferHeader);

class ProducerRingBufferConfigTest : public ::testing::Test {
 protected:
  // A chunk size option as {size_bytes, weight}.
  // A nullopt weight leaves the weight absent.
  using Option = std::pair<uint32_t, std::optional<uint32_t>>;

  static DataSourceConfig MakeConfig(
      std::initializer_list<Option> options,
      std::optional<int32_t> drain_occupancy_percent = std::nullopt) {
    DataSourceConfig config;
    auto* v2_config = config.mutable_experimental_tracing_v2();
    for (const auto& [size, weight] : options) {
      auto* option = v2_config->add_chunk_size_options();
      option->set_size_bytes(size);
      if (weight)
        option->set_weight(*weight);
    }
    if (drain_occupancy_percent)
      v2_config->set_drain_occupancy_percent(*drain_occupancy_percent);
    return config;
  }

  // Resolves |config| with a 32 MiB maximum, the value of kMaxShmSize.
  static std::optional<ProducerRingBufferConfig> Resolve(
      const DataSourceConfig& config,
      size_t shmem_size_hint_bytes) {
    return ResolveProducerRingBufferConfig(config, shmem_size_hint_bytes,
                                           32 * 1024 * 1024);
  }

  // The chunk size chosen from |options|, or 0 if resolution fails.
  static uint32_t ChosenChunkSize(std::initializer_list<Option> options) {
    const auto config = Resolve(MakeConfig(options), /*hint=*/0);
    return config ? config->chunk_size_bytes : 0;
  }
};

// --- Chunk size ---

// Weight 0 means never, and an absent weight counts.
TEST_F(ProducerRingBufferConfigTest, ZeroWeightChunkSizeIsNeverChosen) {
  EXPECT_EQ(ChosenChunkSize({{512, 0}, {1024, std::nullopt}}), 1024u);
}

// Without an option that can be chosen, the chunk size is 256 bytes.
TEST_F(ProducerRingBufferConfigTest, UnusableOptionsUseTheDefaultChunkSize) {
  EXPECT_EQ(ChosenChunkSize({}), kMinChunkSize);
  EXPECT_EQ(ChosenChunkSize({{512, 0}, {1024, 0}}), kMinChunkSize);
  EXPECT_EQ(ChosenChunkSize({{100, std::nullopt}, {258, 5}}), kMinChunkSize);
}

// An invalid size is never chosen.
// The service rejects it, but an older service does not.
TEST_F(ProducerRingBufferConfigTest, InvalidChunkSizeIsNeverChosen) {
  EXPECT_EQ(ChosenChunkSize({{100, 5}, {258, 5}, {2048, 1}}), 2048u);
}

// The total weight exceeds 32 bits. Equal sizes make the result deterministic.
TEST_F(ProducerRingBufferConfigTest, LargeWeightTotalDoesNotOverflow) {
  EXPECT_EQ(
      ChosenChunkSize({{512, std::numeric_limits<uint32_t>::max()}, {512, 1}}),
      512u);
}

// --- Layout ---

// The layout uses the chosen chunk size, also when it is not a power of two.
TEST_F(ProducerRingBufferConfigTest, LayoutUsesChosenChunkSize) {
  // The default 128 KiB hint fits 170 chunks of 768 bytes, rounded down to 128.
  // The threshold must use this count, not a count based on kMinChunkSize.
  const auto config = Resolve(MakeConfig({{768, std::nullopt}}), /*hint=*/0);
  ASSERT_TRUE(config);
  EXPECT_EQ(config->chunk_size_bytes, 768u);
  EXPECT_EQ(config->shmem_size_bytes, kHeader + 128 * 768);
  EXPECT_EQ(config->drain_occupancy_threshold, 32u);

  // A 1 KiB hint holds two chunks of 512 bytes, but only one of 1024 bytes.
  const auto small = Resolve(MakeConfig({{512, std::nullopt}}), 1024);
  ASSERT_TRUE(small);
  EXPECT_EQ(small->shmem_size_bytes, kHeader + 2 * 512);
  EXPECT_FALSE(Resolve(MakeConfig({{1024, std::nullopt}}), 1024));
}

// --- Drain threshold ---

// The threshold is a percent of the chunk count, rounded down, and at least 1.
TEST_F(ProducerRingBufferConfigTest, DrainOccupancyThreshold) {
  struct Case {
    uint32_t num_chunks;
    std::optional<int32_t> percent;
    uint32_t threshold;
  };
  const Case cases[] = {
      // Absent and 0 mean 25%.
      {8, std::nullopt, 2},
      {8, 0, 2},
      // -1 means one position.
      {8, -1, 1},
      {8, 50, 4},
      {8, 100, 8},
      // 99% of 128 is 126.72.
      {128, 99, 126},
      // 1% of 128 is 1.28.
      {128, 1, 1},
      // 25% of 2 rounds down to 0, so the minimum of 1 applies.
      {2, std::nullopt, 1},
  };
  for (size_t i = 0; i < base::ArraySize(cases); ++i) {
    SCOPED_TRACE(i);
    const Case& c = cases[i];
    const auto config =
        Resolve(MakeConfig({}, c.percent), c.num_chunks * kMinChunkSize);
    ASSERT_TRUE(config);
    EXPECT_EQ(config->shmem_size_bytes, kHeader + c.num_chunks * kMinChunkSize);
    EXPECT_EQ(config->drain_occupancy_threshold, c.threshold);
  }
}

// On 64-bit, 100% of kMaxChunksPerRing overflows a 32-bit intermediate product.
// Resolution allocates no shared memory.
TEST_F(ProducerRingBufferConfigTest, DrainThresholdOfLargestRingBuffer) {
  constexpr size_t kMaxSize = std::numeric_limits<size_t>::max();
  for (int32_t percent : {25, 100}) {
    SCOPED_TRACE(percent);
    const auto config = ResolveProducerRingBufferConfig(MakeConfig({}, percent),
                                                        kMaxSize, kMaxSize);
    ASSERT_TRUE(config);
    const size_t num_chunks =
        (config->shmem_size_bytes - kHeader) / kMinChunkSize;
    if (sizeof(size_t) >= sizeof(uint64_t))
      EXPECT_EQ(num_chunks, kMaxChunksPerRing);
    EXPECT_EQ(config->drain_occupancy_threshold,
              percent == 100 ? num_chunks : num_chunks / 4);
  }
}

}  // namespace
}  // namespace perfetto::tracing_v2
