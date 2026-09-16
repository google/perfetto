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

#ifndef INCLUDE_PERFETTO_EXT_BASE_PROGRESS_REPORTER_H_
#define INCLUDE_PERFETTO_EXT_BASE_PROGRESS_REPORTER_H_

#include <cstdint>
#include <mutex>
#include <string_view>

#include "perfetto/base/thread_annotations.h"

namespace perfetto::base {

// True if stderr is an interactive terminal (and TERM is not "dumb"). On WASM
// this is always true: the embedder consumes stderr line by line.
bool StderrSupportsProgress();

// True if ANSI colors should be emitted on stderr. Nonempty FORCE_COLOR forces
// them on, otherwise nonempty NO_COLOR forces them off, otherwise they are
// used when stderr is an interactive terminal.
bool StderrSupportsColor();

// Process-wide, throttled, single-line progress display on stderr. Updates are
// only shown when StderrSupportsProgress(). Log messages clear the line before
// printing, so diagnostics never get merged with progress text.
class ProgressReporter {
 public:
  static ProgressReporter& GetInstance();

  // Suppresses all progress output. Diagnostics are unaffected.
  void set_enabled(bool enabled);

  // |message| must be plain ASCII without newlines or terminal escapes.
  void Update(std::string_view message);
  void Clear();

 private:
  ProgressReporter() = default;
  ~ProgressReporter() = default;
  ProgressReporter(const ProgressReporter&) = delete;
  ProgressReporter& operator=(const ProgressReporter&) = delete;

  void ClearLocked() PERFETTO_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  std::mutex mutex_;
  bool enabled_ PERFETTO_GUARDED_BY(mutex_) = true;
  int64_t last_update_ms_ PERFETTO_GUARDED_BY(mutex_) = 0;
  size_t visible_width_ PERFETTO_GUARDED_BY(mutex_) = 0;
};

}  // namespace perfetto::base
#endif  // INCLUDE_PERFETTO_EXT_BASE_PROGRESS_REPORTER_H_
