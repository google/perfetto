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

#include "perfetto/ext/base/waitable_event.h"

namespace perfetto {
namespace base {

WaitableEvent::WaitableEvent() = default;
WaitableEvent::~WaitableEvent() = default;

void WaitableEvent::Wait(uint64_t notifications) {
  std::unique_lock<std::mutex> lock(mutex_);
  return event_.wait(lock, [&] { return notifications_ >= notifications; });
}

void WaitableEvent::Notify() {
  std::unique_lock<std::mutex> lock(mutex_);
  notifications_++;
  event_.notify_all();
}

}  // namespace base
}  // namespace perfetto
