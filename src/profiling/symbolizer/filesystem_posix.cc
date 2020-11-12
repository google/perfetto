/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/symbolizer/filesystem.h"

#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
#include <fts.h>
#include <sys/stat.h>
#endif
namespace perfetto {
namespace profiling {
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
bool WalkDirectories(std::vector<std::string> dirs, FileCallback fn) {
  std::vector<char*> dir_cstrs;
  for (std::string& dir : dirs)
    dir_cstrs.emplace_back(&dir[0]);
  dir_cstrs.push_back(nullptr);
  base::ScopedResource<FTS*, fts_close, nullptr> fts(
      fts_open(&dir_cstrs[0], FTS_LOGICAL | FTS_NOCHDIR, nullptr));
  if (!fts) {
    PERFETTO_PLOG("fts_open");
    return false;
  }
  FTSENT* ent;
  while ((ent = fts_read(*fts))) {
    if (ent->fts_info & FTS_F)
      fn(ent->fts_path, static_cast<size_t>(ent->fts_statp->st_size));
  }
  return true;
}

size_t GetFileSize(const std::string& file_path) {
  base::ScopedFile fd(base::OpenFile(file_path, O_RDONLY | O_CLOEXEC));
  if (!fd) {
    PERFETTO_PLOG("Failed to get file size %s", file_path.c_str());
    return 0;
  }
  struct stat buf;
  if (fstat(*fd, &buf) == -1) {
    return 0;
  }
  return static_cast<size_t>(buf.st_size);
}
#else
bool WalkDirectories(std::vector<std::string>, FileCallback) {
  return false;
}
size_t GetFileSize(const std::string&) {
  return 0;
}
#endif

}  // namespace profiling
}  // namespace perfetto
