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

#ifndef SRC_TRACE_PROCESSOR_UTIL_SYMBOLIZER_DEBUGINFOD_H_
#define SRC_TRACE_PROCESSOR_UTIL_SYMBOLIZER_DEBUGINFOD_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/util/symbolizer/symbolizer.h"

namespace perfetto::profiling {

// Unset values use the corresponding environment variable. Explicit empty
// values override the environment too. Merely configuring a URL never opts in.
struct DebuginfodOptions {
  bool enabled = false;
  std::optional<std::string> urls;
  std::optional<std::string> cache_path;
  std::string connect_timeout = "5";
  std::string stall_timeout = "10";
};

struct DebuginfodConfig {
  std::vector<std::string> urls;
  std::string cache_path;
  uint32_t connect_timeout_seconds = 5;
  uint32_t stall_timeout_seconds = 10;
};

// Resolves environment defaults and validates configuration. Warnings are
// returned separately so callers can print them even in quiet mode.
base::Status ResolveDebuginfodOptions(const DebuginfodOptions&,
                                      DebuginfodConfig*,
                                      std::string* warnings);

struct DebuginfodStats {
  uint32_t cache_hits = 0;
  uint32_t downloads = 0;
  uint32_t failures = 0;
  std::string details;
  std::string warnings;
};

std::unique_ptr<Symbolizer> CreateDebuginfodSymbolizer(const DebuginfodConfig&,
                                                       bool progress,
                                                       DebuginfodStats*);

}  // namespace perfetto::profiling
#endif  // SRC_TRACE_PROCESSOR_UTIL_SYMBOLIZER_DEBUGINFOD_H_
