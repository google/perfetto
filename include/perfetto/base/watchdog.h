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

#ifndef INCLUDE_PERFETTO_BASE_WATCHDOG_H_
#define INCLUDE_PERFETTO_BASE_WATCHDOG_H_

#include <stdint.h>
#include <time.h>

namespace perfetto {
namespace base {

// WatchDog crashes the calling program using SIGABRT if it does
// not go out of scope within the number of milliseconds passed
// to the constructor.
class WatchDog {
 public:
  explicit WatchDog(time_t millisecs);
  ~WatchDog();

  WatchDog(const WatchDog&) = delete;
  WatchDog& operator=(const WatchDog&) = delete;

 private:
  timer_t timerid_;
};

}  // namespace base
}  // namespace perfetto
#endif
