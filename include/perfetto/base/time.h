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

#ifndef INCLUDE_PERFETTO_BASE_TIME_H_
#define INCLUDE_PERFETTO_BASE_TIME_H_

#include <time.h>

#include <chrono>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)
#include <mach/mach_time.h>
#endif

namespace perfetto {
namespace base {

using TimeSeconds = std::chrono::seconds;
using TimeMillis = std::chrono::milliseconds;
using TimeNanos = std::chrono::nanoseconds;

inline TimeNanos FromPosixTimespec(const struct timespec& ts) {
  return TimeNanos(ts.tv_sec * 1000000000LL + ts.tv_nsec);
}

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)

constexpr clockid_t kWallTimeClockSource = CLOCK_MONOTONIC;

inline TimeNanos GetTimeInternalNs(clockid_t clk_id) {
  struct timespec ts = {};
  PERFETTO_CHECK(clock_gettime(clk_id, &ts) == 0);
  return FromPosixTimespec(ts);
}

inline TimeNanos GetWallTimeNs() {
  return GetTimeInternalNs(kWallTimeClockSource);
}

inline TimeNanos GetThreadCPUTimeNs() {
  return GetTimeInternalNs(CLOCK_THREAD_CPUTIME_ID);
}

#else  // !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)

inline TimeNanos GetWallTimeNs() {
  auto init_time_factor = []() -> uint64_t {
    mach_timebase_info_data_t timebase_info;
    mach_timebase_info(&timebase_info);
    return timebase_info.numer / timebase_info.denom;
  };

  static uint64_t monotonic_timebase_factor = init_time_factor();
  return TimeNanos(mach_absolute_time() * monotonic_timebase_factor);
}

#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX)

inline TimeMillis GetWallTimeMs() {
  return std::chrono::duration_cast<TimeMillis>(GetWallTimeNs());
}

inline TimeSeconds GetWallTimeS() {
  return std::chrono::duration_cast<TimeSeconds>(GetWallTimeNs());
}

inline struct timespec ToPosixTimespec(TimeMillis time) {
  struct timespec ts {};
  const long time_s = static_cast<long>(time.count() / 1000);
  ts.tv_sec = time_s;
  ts.tv_nsec = (static_cast<long>(time.count()) - time_s * 1000L) * 1000000L;
  return ts;
}

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_TIME_H_
