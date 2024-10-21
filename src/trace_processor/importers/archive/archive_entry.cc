/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/archive/archive_entry.h"

#include <tuple>

namespace perfetto::trace_processor {

bool ArchiveEntry::operator<(const ArchiveEntry& rhs) const {
  // Traces with symbols should be the last ones to be read.
  // TODO(carlscab): Proto traces with just ModuleSymbols packets should be an
  // exception. We actually need those are the very end (once whe have all the
  // Frames). Alternatively we could build a map address -> symbol during
  // tokenization and use this during parsing to resolve symbols.
  if (trace_type == kSymbolsTraceType) {
    return false;
  }
  if (rhs.trace_type == kSymbolsTraceType) {
    return true;
  }

  // Proto traces should always parsed first as they might contains clock sync
  // data needed to correctly parse other traces.
  if (rhs.trace_type == TraceType::kProtoTraceType) {
    return false;
  }
  if (trace_type == TraceType::kProtoTraceType) {
    return true;
  }

  if (rhs.trace_type == TraceType::kGzipTraceType) {
    return false;
  }
  if (trace_type == TraceType::kGzipTraceType) {
    return true;
  }

  return std::tie(name, index) < std::tie(rhs.name, rhs.index);
}

}  // namespace perfetto::trace_processor
