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
#include <string>

namespace perfetto::base {

bool StderrSupportsProgress();
bool StderrSupportsColor();

// A single-line, throttled stderr display. Updates must be plain ASCII without
// terminal escapes. No output is produced for redirected stderr or TERM=dumb.
// Use Clear() before ordinary stdio diagnostics; Perfetto logs clear it
// automatically. This class does not print a final summary.
class ProgressReporter {
 public:
  explicit ProgressReporter(bool enabled = true);
  ~ProgressReporter();
  ProgressReporter(const ProgressReporter&) = delete;
  ProgressReporter& operator=(const ProgressReporter&) = delete;

  void Update(const std::string& message);
  void Clear();
  static void ClearBeforeLog();

 private:
  void ClearLocked();
  bool enabled_;
  int64_t last_update_ms_ = 0;
  size_t visible_width_ = 0;
};

}  // namespace perfetto::base
#endif  // INCLUDE_PERFETTO_EXT_BASE_PROGRESS_REPORTER_H_
