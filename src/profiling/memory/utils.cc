/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/memory/utils.h"

namespace perfetto {
namespace profiling {

// Behaves as a pread64, emulating it if not already exposed by the standard
// library. Safe to use on 32bit platforms for addresses with the top bit set.
// Clobbers the |fd| seek position if emulating.
ssize_t ReadAtOffsetClobberSeekPos(int fd,
                                   void* buf,
                                   size_t count,
                                   off64_t addr) {
#ifdef __BIONIC__
  return pread64(fd, buf, count, addr);
#else
  if (lseek64(fd, addr, SEEK_SET) == -1)
    return -1;
  return read(fd, buf, count);
#endif
}

}  // namespace profiling
}  // namespace perfetto
