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

#include "src/trace_processor/file_reader.h"

#include <fcntl.h>
#include <sys/stat.h>

namespace perfetto {
namespace trace_processor {

FileReader::FileReader(const char* path) {
  fd_.reset(open(path, O_RDONLY));
  if (!fd_)
    PERFETTO_FATAL("Could not open %s", path);
  struct stat stat_buf {};
  PERFETTO_CHECK(fstat(*fd_, &stat_buf) == 0);
  file_size_ = static_cast<uint64_t>(stat_buf.st_size);
}

FileReader::~FileReader() = default;

uint32_t FileReader::Read(uint64_t offset, uint32_t len, uint8_t* dst) {
  ssize_t res = pread(*fd_, dst, len, static_cast<off_t>(offset));
  return res > 0 ? static_cast<uint32_t>(res) : 0;
}

}  // namespace trace_processor
}  // namespace perfetto
