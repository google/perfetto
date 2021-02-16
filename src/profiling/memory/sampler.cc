/*
 * Copyright (C) 2020 The Android Open Source Project
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

namespace perfetto {
namespace profiling {

namespace {

// If the probability of getting less than one sample is less than this,
// sidestep the sampler and treat the allocation as a sample.
constexpr double kPassthroughError = 0.01;

}  // namespace

uint64_t GetPassthroughThreshold(uint64_t interval) {
  if (interval <= 1)
    return interval;
  // (1 - 1 / interval)^x = kPassthroughError
  // x = log_(1 - 1/interval)(kPassthroughError)
  return 1 + uint64_t(log(kPassthroughError) / log(1.0 - 1 / double(interval)));
}

std::default_random_engine& GetGlobalRandomEngineLocked() {
  static std::default_random_engine engine;
  return engine;
}

void Sampler::SetSamplingInterval(uint64_t sampling_interval) {
  sampling_interval_ = sampling_interval;
  passthrough_threshold_ = GetPassthroughThreshold(sampling_interval_);
  sampling_rate_ = 1.0 / static_cast<double>(sampling_interval_);
  interval_to_next_sample_ = NextSampleInterval();
}

}  // namespace profiling
}  // namespace perfetto
