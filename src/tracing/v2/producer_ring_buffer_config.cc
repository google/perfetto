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

#include <algorithm>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"

namespace perfetto::tracing_v2 {
namespace {

uint32_t ChooseChunkSize(const DataSourceConfig& config) {
  using ChunkSizeOption =
      DataSourceConfig::ExperimentalTracingV2Config::ChunkSizeOption;

  const auto& options = config.experimental_tracing_v2().chunk_size_options();
  auto weight_of = [](const ChunkSizeOption& option) -> uint64_t {
    if (!IsValidChunkSize(option.size_bytes()))
      return 0;
    return option.has_weight() ? option.weight() : 1;
  };

  uint64_t total_weight = 0;
  for (const ChunkSizeOption& option : options)
    total_weight += weight_of(option);

  if (total_weight == 0)
    return kMinChunkSize;

  // Walk the options until their cumulative weight passes |target|.
  const uint64_t target =
      static_cast<uint64_t>(base::Uuidv4().lsb()) % total_weight;
  uint64_t cumulative_weight = 0;
  for (const ChunkSizeOption& option : options) {
    cumulative_weight += weight_of(option);
    if (target < cumulative_weight)
      return option.size_bytes();
  }

  // Not reached: the last cumulative weight is the total, which is above
  // |target|, so the loop returns.
  PERFETTO_DFATAL("ChooseChunkSize: no option for the target");
  return kMinChunkSize;
}

// Returns the drain threshold, in positions, for a ring buffer of
// |num_chunks| chunks.
// |drain_occupancy_percent| must be -1 to 100:
// - -1: 1, so a writer asks for a drain after every publication.
// - 0: |kDefaultDrainOccupancyPercent|% of |num_chunks|.
// - 1 to 100: that percent of |num_chunks|.
// The result is at least 1.
uint32_t ComputeDrainOccupancyThreshold(uint32_t num_chunks,
                                        int32_t drain_occupancy_percent) {
  constexpr uint32_t kDefaultDrainOccupancyPercent = 25;

  PERFETTO_DCHECK(drain_occupancy_percent >= -1 &&
                  drain_occupancy_percent <= 100);
  if (drain_occupancy_percent == -1)
    return 1;
  const uint64_t percent = drain_occupancy_percent == 0
                               ? kDefaultDrainOccupancyPercent
                               : static_cast<uint64_t>(drain_occupancy_percent);
  const uint64_t threshold = uint64_t{num_chunks} * percent / 100;
  return std::max(1u, static_cast<uint32_t>(threshold));
}

}  // namespace

std::optional<ProducerRingBufferConfig> ResolveProducerRingBufferConfig(
    const DataSourceConfig& config,
    size_t shmem_size_hint_bytes,
    size_t max_shmem_size_bytes) {
  const uint32_t chunk_size_bytes = ChooseChunkSize(config);
  const auto shmem_size_bytes = RingBufferSizeForShmSizeHint(
      shmem_size_hint_bytes, max_shmem_size_bytes, chunk_size_bytes);
  if (!shmem_size_bytes)
    return std::nullopt;

  const uint32_t num_chunks = static_cast<uint32_t>(
      (shmem_size_bytes.value() - sizeof(RingBufferHeader)) / chunk_size_bytes);
  const uint32_t drain_occupancy_threshold = ComputeDrainOccupancyThreshold(
      num_chunks, config.experimental_tracing_v2().drain_occupancy_percent());

  return ProducerRingBufferConfig{shmem_size_bytes.value(), chunk_size_bytes,
                                  drain_occupancy_threshold};
}

}  // namespace perfetto::tracing_v2
