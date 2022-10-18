/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/sqlite/sqlite_utils.h"

namespace perfetto {
namespace trace_processor {
namespace sqlite_utils {

std::wstring SqliteValueToWString(sqlite3_value* value) {
  PERFETTO_CHECK(sqlite3_value_type(value) == SQLITE_TEXT);
  int len = sqlite3_value_bytes16(value);
  PERFETTO_CHECK(len >= 0);
  size_t count = static_cast<size_t>(len) / sizeof(wchar_t);
  return std::wstring(
      reinterpret_cast<const wchar_t*>(sqlite3_value_text16(value)), count);
}

}  // namespace sqlite_utils
}  // namespace trace_processor
}  // namespace perfetto
