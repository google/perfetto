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

#include "src/trace_processor/shell/result_formatter.h"

#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/trace_processor/shell/csv_result_formatter.h"

namespace perfetto::trace_processor {

ResultFormatter::~ResultFormatter() = default;

std::unique_ptr<ResultFormatter> CreateResultFormatter(
    const ResultFormatOptions& options,
    std::vector<std::string> column_names,
    FILE* output) {
  switch (options.format) {
    case ResultFormat::kCsv:
      return std::make_unique<CsvResultFormatter>(std::move(column_names),
                                                  output);
  }
  return nullptr;
}

}  // namespace perfetto::trace_processor
