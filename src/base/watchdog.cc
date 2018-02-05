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

#include "perfetto/base/watchdog.h"

#include "perfetto/base/logging.h"

#include <signal.h>
#include <stdint.h>

namespace perfetto {
namespace base {

WatchDog::WatchDog(time_t millisecs) {
  struct sigevent sev = {};
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGABRT;
  PERFETTO_CHECK(timer_create(CLOCK_MONOTONIC, &sev, &timerid_) != -1);
  struct itimerspec its = {};
  its.it_value.tv_sec = millisecs / 1000;
  its.it_value.tv_nsec = 1000000L * (millisecs % 1000);
  PERFETTO_CHECK(timer_settime(timerid_, 0, &its, nullptr) != -1);
}

WatchDog::~WatchDog() {
  PERFETTO_CHECK(timer_delete(timerid_) != -1);
}

}  // namespace base
}  // namespace perfetto
