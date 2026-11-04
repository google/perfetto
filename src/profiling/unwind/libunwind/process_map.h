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

#ifndef SRC_PROFILING_UNWIND_LIBUNWIND_PROCESS_MAP_H_
#define SRC_PROFILING_UNWIND_LIBUNWIND_PROCESS_MAP_H_

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <string>

namespace perfetto {
namespace profiling {

// One line of /proc/<pid>/maps, e.g.:
//   00400000-00409000 r-xp 00001000 fc:00 426998  /usr/lib/gvfs/gvfsd-http
//   ^start   ^end     ^perm ^pgoff  ^dev  ^inode  ^name (optional)
//
// A dependency-free replacement for android::procinfo::MapInfo /
// ReadMapFileContent() from platform/system/libprocinfo.
struct ProcMapsEntry {
  uint64_t start = 0;
  uint64_t end = 0;
  // Bitwise OR of PROT_READ / PROT_WRITE / PROT_EXEC.
  uint16_t flags = 0;
  // 's' (shared) vs 'p' (private) in the permission column.
  bool shared = false;
  uint64_t pgoff = 0;
  ino_t inode = 0;
  std::string name;
};

// Parses a single, '\n'-free maps line. Returns false on malformed input.
bool ParseProcMapsLine(const char* line, ProcMapsEntry* out);

// Parses the full content of /proc/<pid>/maps, invoking |callback| once per
// line in file order. Stops and returns false at the first malformed line.
bool ParseProcMaps(const std::string& content,
                   const std::function<void(const ProcMapsEntry&)>& callback);

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_LIBUNWIND_PROCESS_MAP_H_
