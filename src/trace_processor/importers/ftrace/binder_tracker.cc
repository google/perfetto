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

#include "src/trace_processor/importers/ftrace/binder_tracker.h"

#include "perfetto/base/compiler.h"

namespace perfetto {
namespace trace_processor {

BinderTracker::BinderTracker(TraceProcessorContext* context)
    : context_(context) {}

BinderTracker::~BinderTracker() = default;

void BinderTracker::Transaction(int64_t ts, uint32_t pid) {
  base::ignore_result(context_);
  base::ignore_result(ts);
  base::ignore_result(pid);
}

void BinderTracker::Lock(int64_t ts, uint32_t pid) {
  base::ignore_result(ts);
  base::ignore_result(pid);
}

void BinderTracker::Locked(int64_t ts, uint32_t pid) {
  base::ignore_result(ts);
  base::ignore_result(pid);
}

void BinderTracker::Unlock(int64_t ts, uint32_t pid) {
  base::ignore_result(ts);
  base::ignore_result(pid);
}

void BinderTracker::TransactionReceived(int64_t ts, uint32_t pid) {
  base::ignore_result(ts);
  base::ignore_result(pid);
}

void BinderTracker::TransactionAllocBuf(int64_t ts, uint32_t pid) {
  base::ignore_result(ts);
  base::ignore_result(pid);
}

}  // namespace trace_processor
}  // namespace perfetto
