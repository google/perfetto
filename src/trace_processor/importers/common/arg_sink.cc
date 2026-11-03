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

#include "src/trace_processor/importers/common/arg_sink.h"

#include <cstddef>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

ArgSink::~ArgSink() = default;
ArgsInserterSink::~ArgsInserterSink() = default;

void ArgsInserterSink::Add(ArgKey key, Variadic value) {
  inserter_.AddArg(key.flat_key, key.key, value);
}

size_t ArgsInserterSink::ArrayIndex(StringPool::Id key) {
  return inserter_.GetNextArrayEntryIndex(key);
}

size_t ArgsInserterSink::IncrementArrayIndex(StringPool::Id key) {
  return inserter_.IncrementArrayEntryIndex(key);
}

}  // namespace perfetto::trace_processor
