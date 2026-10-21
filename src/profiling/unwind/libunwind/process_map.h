/*
 * Copyright (C) 2018 The Android Open Source Project
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

/*
 * copied from platform/system/libprocinfo/include/procinfo/process_map.h.
 * without pulling any android dependency.
 */

#ifndef SRC_PROFILING_UNWIND_LIBUNWIND_PROCESS_MAP_H_
#define SRC_PROFILING_UNWIND_LIBUNWIND_PROCESS_MAP_H_

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <functional>
#include <string>

namespace perfetto {
namespace profiling {
namespace procinfo {

// Disabling the clang specific diagnostic warnings to keep the header as close
// to the upstream as possible.

// clang-format off
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshadow-field-in-constructor"
#pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
#endif

struct MapInfo {
  uint64_t start;
  uint64_t end;
  uint16_t flags;
  uint64_t pgoff;
  ino_t inode;
  std::string name;
  bool shared;

  MapInfo(uint64_t start, uint64_t end, uint16_t flags, uint64_t pgoff, ino_t inode,
          const char* name, bool shared)
      : start(start),
        end(end),
        flags(flags),
        pgoff(pgoff),
        inode(inode),
        name(name),
        shared(shared) {}

  MapInfo(const MapInfo& params)
      : start(params.start),
        end(params.end),
        flags(params.flags),
        pgoff(params.pgoff),
        inode(params.inode),
        name(params.name),
        shared(params.shared) {}
};

typedef std::function<void(const MapInfo&)> MapInfoCallback;
typedef std::function<void(uint64_t start, uint64_t end, uint16_t flags, uint64_t pgoff,
                      ino_t inode, const char* name, bool shared)> MapInfoParamsCallback;

static inline bool PassSpace(char** p) {
  if (**p != ' ') {
    return false;
  }
  while (**p == ' ') {
    (*p)++;
  }
  return true;
}

static inline bool PassXdigit(char** p) {
  if (!isxdigit(**p)) {
    return false;
  }
  do {
    (*p)++;
  } while (isxdigit(**p));
  return true;
}

// Parses a line given p pointing at proc/<pid>/maps content buffer and returns true on success
// and false on failure parsing. The next end of line will be replaced by null character and the
// immediate offset after the parsed line will be returned in next_line.
//
// Example of how a parsed line look line:
// 00400000-00409000 r-xp 00000000 fc:00 426998  /usr/lib/gvfs/gvfsd-http
static inline bool ParseMapsFileLine(char* p, uint64_t& start_addr, uint64_t& end_addr, uint16_t& flags,
                      uint64_t& pgoff, ino_t& inode, char** name, bool& shared, char** next_line) {
  // Make end of line be null
  *next_line = strchr(p, '\n');
  if (*next_line != nullptr) {
    **next_line = '\0';
    (*next_line)++;
  }

  char* end;
  // start_addr
  start_addr = strtoull(p, &end, 16);
  if (end == p || *end != '-') {
    return false;
  }
  p = end + 1;
  // end_addr
  end_addr = strtoull(p, &end, 16);
  if (end == p) {
    return false;
  }
  p = end;
  if (!PassSpace(&p)) {
    return false;
  }
  // flags
  flags = 0;
  if (*p == 'r') {
    flags |= PROT_READ;
  } else if (*p != '-') {
    return false;
  }
  p++;
  if (*p == 'w') {
    flags |= PROT_WRITE;
  } else if (*p != '-') {
    return false;
  }
  p++;
  if (*p == 'x') {
    flags |= PROT_EXEC;
  } else if (*p != '-') {
    return false;
  }
  p++;
  if (*p != 'p' && *p != 's') {
    return false;
  }
  shared = *p == 's';

  p++;
  if (!PassSpace(&p)) {
    return false;
  }
  // pgoff
  pgoff = strtoull(p, &end, 16);
  if (end == p) {
    return false;
  }
  p = end;
  if (!PassSpace(&p)) {
    return false;
  }
  // major:minor
  if (!PassXdigit(&p) || *p++ != ':' || !PassXdigit(&p) || !PassSpace(&p)) {
    return false;
  }
  // inode
  inode = strtoull(p, &end, 10);
  if (end == p) {
    return false;
  }
  p = end;

  if (*p != '\0' && !PassSpace(&p)) {
    return false;
  }

  *name = p;

  return true;
}

inline bool ReadMapFileContent(char* content, const MapInfoParamsCallback& callback) {
  uint64_t start_addr;
  uint64_t end_addr;
  uint16_t flags;
  uint64_t pgoff;
  ino_t inode;
  char* line_start = content;
  char* next_line;
  char* name;
  bool shared;

  while (line_start != nullptr && *line_start != '\0') {
    bool parsed = ParseMapsFileLine(line_start, start_addr, end_addr, flags, pgoff,
                                    inode, &name, shared, &next_line);
    if (!parsed) {
      return false;
    }

    line_start = next_line;
    callback(start_addr, end_addr, flags, pgoff, inode, name, shared);
  }
  return true;
}

inline bool ReadMapFileContent(char* content, const MapInfoCallback& callback) {
  uint64_t start_addr;
  uint64_t end_addr;
  uint16_t flags;
  uint64_t pgoff;
  ino_t inode;
  char* line_start = content;
  char* next_line;
  char* name;
  bool shared;

  while (line_start != nullptr && *line_start != '\0') {
    bool parsed = ParseMapsFileLine(line_start, start_addr, end_addr, flags, pgoff,
                                    inode, &name, shared, &next_line);
    if (!parsed) {
      return false;
    }

    line_start = next_line;
    callback(MapInfo(start_addr, end_addr, flags, pgoff, inode, name, shared));
  }
  return true;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// clang-format on

}  // namespace procinfo
}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_LIBUNWIND_PROCESS_MAP_H_
