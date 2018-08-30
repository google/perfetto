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

#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>

#include <procinfo/process_map.h>

#include "perfetto/base/file_utils.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/scoped_file.h"
#include "src/profiling/memory/transport_data.h"
#include "src/profiling/memory/unwinding.h"

namespace perfetto {

StackMemory::StackMemory(int mem_fd, uint64_t sp, uint8_t* stack, size_t size)
    : mem_fd_(mem_fd), sp_(sp), stack_end_(sp + size), stack_(stack) {}

size_t StackMemory::Read(uint64_t addr, void* dst, size_t size) {
  if (addr >= sp_ && addr + size <= stack_end_ && addr + size > sp_) {
    size_t offset = static_cast<size_t>(addr - sp_);
    memcpy(dst, stack_ + offset, size);
    return size;
  }

  if (lseek(mem_fd_, static_cast<off_t>(addr), SEEK_SET) == -1)
    return 0;

  ssize_t rd = read(mem_fd_, dst, size);
  if (rd == -1) {
    PERFETTO_DPLOG("read");
    return 0;
  }
  return static_cast<size_t>(rd);
}

FileDescriptorMaps::FileDescriptorMaps(base::ScopedFile fd)
    : fd_(std::move(fd)) {}

bool FileDescriptorMaps::Parse() {
  // If the process has already exited, lseek or ReadFileDescriptor will
  // return false.
  if (lseek(*fd_, 0, SEEK_SET) == -1)
    return false;

  std::string content;
  if (!base::ReadFileDescriptor(*fd_, &content))
    return false;
  PERFETTO_DLOG("%s", content.c_str());
  return android::procinfo::ReadMapFileContent(
      &content[0], [&](uint64_t start, uint64_t end, uint16_t flags,
                       uint64_t pgoff, const char* name) {
        // Mark a device map in /dev/ and not in /dev/ashmem/ specially.
        if (strncmp(name, "/dev/", 5) == 0 &&
            strncmp(name + 5, "ashmem/", 7) != 0) {
          flags |= unwindstack::MAPS_FLAGS_DEVICE_MAP;
        }
        maps_.push_back(
            new unwindstack::MapInfo(start, end, pgoff, flags, name));
      });
}

void FileDescriptorMaps::Reset() {
  for (unwindstack::MapInfo* info : maps_)
    delete info;
  maps_.clear();
}

}  // namespace perfetto
