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

#include "src/profiling/unwind/libunwind/process_map.h"

#include <sys/mman.h>

#include <cctype>
#include <cstdlib>

#include "perfetto/ext/base/string_splitter.h"

namespace perfetto {
namespace profiling {
namespace {

// Consumes |c| at |*p|. Returns false if the next char is something else.
bool Expect(const char** p, char c) {
  if (**p != c)
    return false;
  ++*p;
  return true;
}

// Advances |*p| past one or more spaces. Returns false if there is none.
bool SkipSpaces(const char** p) {
  if (**p != ' ')
    return false;
  while (**p == ' ')
    ++*p;
  return true;
}

// Advances |*p| past one or more hex digits. Returns false if there is none.
bool SkipHexDigits(const char** p) {
  if (!isxdigit(static_cast<unsigned char>(**p)))
    return false;
  while (isxdigit(static_cast<unsigned char>(**p)))
    ++*p;
  return true;
}

// Parses an unsigned integer in |base| at |*p| into |*out| and advances |*p|
// past it. Returns false if no digits were consumed.
bool ParseUInt(const char** p, int base, uint64_t* out) {
  char* end = nullptr;
  *out = strtoull(*p, &end, base);
  if (end == *p)
    return false;
  *p = end;
  return true;
}

// Parses the 4-char "rwxp" permission column.
bool ParsePerms(const char** p, uint16_t* flags, bool* shared) {
  static constexpr struct {
    char ch;
    uint16_t flag;
  } kProt[] = {{'r', PROT_READ}, {'w', PROT_WRITE}, {'x', PROT_EXEC}};

  *flags = 0;
  for (const auto& prot : kProt) {
    if (**p == prot.ch) {
      *flags |= prot.flag;
    } else if (**p != '-') {
      return false;
    }
    ++*p;
  }
  if (**p != 'p' && **p != 's')
    return false;
  *shared = (**p == 's');
  ++*p;
  return true;
}

}  // namespace

bool ParseProcMapsLine(const char* p, ProcMapsEntry* out) {
  // start-end
  if (!ParseUInt(&p, 16, &out->start) || !Expect(&p, '-') ||
      !ParseUInt(&p, 16, &out->end) || !SkipSpaces(&p)) {
    return false;
  }
  // rwxp
  if (!ParsePerms(&p, &out->flags, &out->shared) || !SkipSpaces(&p))
    return false;
  // pgoff
  if (!ParseUInt(&p, 16, &out->pgoff) || !SkipSpaces(&p))
    return false;
  // major:minor
  if (!SkipHexDigits(&p) || !Expect(&p, ':') || !SkipHexDigits(&p) ||
      !SkipSpaces(&p)) {
    return false;
  }
  // inode
  uint64_t inode = 0;
  if (!ParseUInt(&p, 10, &inode))
    return false;
  out->inode = static_cast<ino_t>(inode);
  // name (optional, preceded by padding spaces)
  if (*p != '\0' && !SkipSpaces(&p))
    return false;
  out->name.assign(p);
  return true;
}

bool ParseProcMaps(const std::string& content,
                   const std::function<void(const ProcMapsEntry&)>& callback) {
  // StringSplitter takes a copy so it can null-terminate tokens in place.
  ProcMapsEntry entry;
  for (base::StringSplitter lines(content, '\n'); lines.Next();) {
    if (!ParseProcMapsLine(lines.cur_token(), &entry))
      return false;
    callback(entry);
  }
  return true;
}

}  // namespace profiling
}  // namespace perfetto
