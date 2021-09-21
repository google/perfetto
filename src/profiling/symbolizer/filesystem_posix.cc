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

#include "perfetto/base/build_config.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
#include <sys/stat.h>
#endif

#include <string>

#include "perfetto/ext/base/file_utils.h"

namespace perfetto {
namespace profiling {
#if PERFETTO_BUILDFLAG(PERFETTO_LOCAL_SYMBOLIZER)
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
size_t GetFileSize(const std::string&) {
  return 0;
}
#endif

}  // namespace profiling
}  // namespace perfetto

#endif  // !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
