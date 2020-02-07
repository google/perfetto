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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_BINDER_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_BINDER_TRACKER_H_

#include <stdint.h>

#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class BinderTracker {
 public:
  explicit BinderTracker(TraceProcessorContext*);
  virtual ~BinderTracker();

  void Transaction(int64_t timestamp, uint32_t pid);
  void Locked(int64_t timestamp, uint32_t pid);
  void Lock(int64_t timestamp, uint32_t pid);
  void Unlock(int64_t timestamp, uint32_t pid);
  void TransactionReceived(int64_t timestamp, uint32_t pid);
  void TransactionAllocBuf(int64_t timestamp, uint32_t pid);

 private:
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FTRACE_BINDER_TRACKER_H_
