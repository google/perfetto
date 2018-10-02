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

#ifndef SRC_PROFILING_MEMORY_SAMPLER_H_
#define SRC_PROFILING_MEMORY_SAMPLER_H_

#include <pthread.h>
#include <stdint.h>

#include <random>

namespace perfetto {

// This is the thread-local state needed to apply poission sampling to malloc
// samples.
//
// We apply poisson sampling individually to each byte. The whole
// allocation gets accounted as often as the number of sampled bytes it
// contains.
//
// Googlers see go/chrome-shp for more details about the sampling (from
// Chrome's heap profiler).
class ThreadLocalSamplingData {
 public:
  ThreadLocalSamplingData(void (*unhooked_free)(void*))
      : unhooked_free_(unhooked_free) {}
  // Returns number of times a sample should be accounted. Due to how the
  // poission sampling works, some samples should be accounted multiple times.
  size_t ShouldSample(size_t sz, double rate);

  // Destroy a TheadLocalSamplingData object after the pthread key has been
  // deleted or when the thread shuts down. This uses unhooked_free passed in
  // the constructor.
  static void KeyDestructor(void* ptr);

 private:
  int64_t NextSampleInterval(double rate);
  void (*unhooked_free_)(void*);
  int64_t interval_to_next_sample_ = 0;
  std::default_random_engine random_engine_;
};

// Returns number of times a sample should be accounted. Due to how the
// poission sampling works, some samples should be accounted multiple times.
//
// Delegate to this thread's ThreadLocalSamplingData.
//
// We have to pass through the real malloc in order to allocate the TLS.
size_t ShouldSample(pthread_key_t key,
                    size_t sz,
                    double rate,
                    void* (*unhooked_malloc)(size_t),
                    void (*unhooked_free)(void*));

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_SAMPLER_H_
