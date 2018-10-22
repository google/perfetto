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

#include "perfetto/base/utils.h"

namespace perfetto {
namespace profiling {
namespace {
ThreadLocalSamplingData* GetSpecific(pthread_key_t key,
                                     uint64_t interval,
                                     void* (*unhooked_malloc)(size_t),
                                     void (*unhooked_free)(void*)) {
  // This should not be used with glibc as it might re-enter into malloc, see
  // http://crbug.com/776475.
  void* specific = pthread_getspecific(key);
  if (specific == nullptr) {
    specific = unhooked_malloc(sizeof(ThreadLocalSamplingData));
    new (specific) ThreadLocalSamplingData(unhooked_free, interval);
    pthread_setspecific(key, specific);
  }
  return reinterpret_cast<ThreadLocalSamplingData*>(specific);
}
}  // namespace

// The algorithm below is inspired by the Chromium sampling algorithm at
// https://cs.chromium.org/search/?q=f:cc+symbol:AllocatorShimLogAlloc+package:%5Echromium$&type=cs

int64_t ThreadLocalSamplingData::NextSampleInterval() {
  std::exponential_distribution<double> dist(rate_);
  int64_t next = static_cast<int64_t>(dist(random_engine_));
  // The +1 corrects the distribution of the first value in the interval.
  // TODO(fmayer): Figure out why.
  return next + 1;
}

size_t ThreadLocalSamplingData::NumberOfSamples(size_t sz) {
  interval_to_next_sample_ -= sz;
  size_t sz_multiplier = 0;
  while (PERFETTO_UNLIKELY(interval_to_next_sample_ <= 0)) {
    interval_to_next_sample_ += NextSampleInterval();
    ++sz_multiplier;
  }
  return sz_multiplier;
}

std::atomic<uint64_t> ThreadLocalSamplingData::seed(1);

size_t SampleSize(pthread_key_t key,
                  size_t sz,
                  uint64_t interval,
                  void* (*unhooked_malloc)(size_t),
                  void (*unhooked_free)(void*)) {
  if (PERFETTO_UNLIKELY(sz >= interval))
    return sz;
  return interval * GetSpecific(key, interval, unhooked_malloc, unhooked_free)
                        ->NumberOfSamples(sz);
}

void ThreadLocalSamplingData::KeyDestructor(void* ptr) {
  ThreadLocalSamplingData* thread_local_data =
      reinterpret_cast<ThreadLocalSamplingData*>(ptr);
  void (*unhooked_free)(void*) = thread_local_data->unhooked_free_;
  thread_local_data->~ThreadLocalSamplingData();
  unhooked_free(ptr);
}

}  // namespace profiling
}  // namespace perfetto
