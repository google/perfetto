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

#include "src/trace_processor/sqlite/sqlite_database.h"

#include <atomic>
#include <cstdint>

#include "perfetto/ext/base/string_utils.h"

namespace perfetto::trace_processor {
namespace {

uint64_t NextDatabaseId() {
  static std::atomic<uint64_t> next{0};
  return next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

SqliteDatabase::SqliteDatabase()
    : shared_filename_("file:/perfetto-" +
                       base::Uint64ToHexStringNoPrefix(NextDatabaseId()) +
                       "?vfs=memdb") {}

SqliteDatabase::~SqliteDatabase() = default;

}  // namespace perfetto::trace_processor
