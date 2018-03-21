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

#include <sys/stat.h>

#include "perfetto/base/file_utils.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"

namespace perfetto {
namespace base {
namespace {
constexpr size_t kBufSize = 2048;
}

bool ReadFile(const std::string& path, std::string* out) {
  // Do not override existing data in string.
  size_t i = out->size();

  base::ScopedFile fd = base::OpenFile(path.c_str(), O_RDONLY);
  if (!fd)
    return false;

  struct stat buf {};
  if (fstat(*fd, &buf) != -1) {
    if (buf.st_size > 0)
      out->resize(i + static_cast<size_t>(buf.st_size));
  }

  ssize_t bytes_read;
  for (;;) {
    if (out->size() < i + kBufSize)
      out->resize(out->size() + kBufSize);

    bytes_read = PERFETTO_EINTR(read(fd.get(), &((*out)[i]), kBufSize));
    if (bytes_read > 0) {
      i += static_cast<size_t>(bytes_read);
    } else {
      out->resize(i);
      return bytes_read == 0;
    }
  }
}

}  // namespace base
}  // namespace perfetto
