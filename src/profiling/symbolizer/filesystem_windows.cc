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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

#include <Windows.h>

namespace perfetto {
namespace profiling {

size_t GetFileSize(const std::string& file_path) {
  HANDLE file =
      CreateFileA(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    PERFETTO_PLOG("Failed to get file size %s", file_path.c_str());
    return 0;
  }
  LARGE_INTEGER file_size;
  file_size.QuadPart = 0;
  if (!GetFileSizeEx(file, &file_size)) {
    PERFETTO_PLOG("Failed to get file size %s", file_path.c_str());
  }
  CloseHandle(file);
  return static_cast<size_t>(file_size.QuadPart);
}

}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
