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
#ifndef PRODUCTS_MEMSCOPE_CLI_REPORT_H_
#define PRODUCTS_MEMSCOPE_CLI_REPORT_H_

#include <optional>
#include <string>

namespace perfetto::memscope {

struct ReportArgs {
  std::string trace_path;
  // Process to report on; the highest scoring one when neither is set.
  std::optional<int64_t> pid;
  std::string app;
};

// Prints the Memory Explorer summary numbers for one process of a trace,
// computed with the same `memscope` PerfettoSQL package the UI uses.
int PrintReport(const ReportArgs& args);

}  // namespace perfetto::memscope

#endif  // PRODUCTS_MEMSCOPE_CLI_REPORT_H_
