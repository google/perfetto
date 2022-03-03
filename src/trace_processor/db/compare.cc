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

#include "src/trace_processor/db/compare.h"

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"

#if PERFETTO_BUILDFLAG(PERFETTO_TP_SQLITE)
#include <sqlite3.h>
#endif

namespace perfetto {
namespace trace_processor {
namespace compare {

int Glob(NullTermStringView value, NullTermStringView pattern) {
#if PERFETTO_BUILDFLAG(PERFETTO_TP_SQLITE)
  return sqlite3_strglob(pattern.c_str(), value.c_str());
#else
  if (value == pattern)
    return 0;

  PERFETTO_FATAL("Glob not supported when SQLite not available");
#endif
}

}  // namespace compare
}  // namespace trace_processor
}  // namespace perfetto
