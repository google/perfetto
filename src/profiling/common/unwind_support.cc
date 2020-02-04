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

#include "src/profiling/common/unwind_support.h"

#include <inttypes.h>

#include <procinfo/process_map.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>

#include "perfetto/ext/base/file_utils.h"

namespace perfetto {
namespace profiling {

StackOverlayMemory::StackOverlayMemory(std::shared_ptr<unwindstack::Memory> mem,
                                       uint64_t sp,
                                       uint8_t* stack,
                                       size_t size)
    : mem_(std::move(mem)), sp_(sp), stack_end_(sp + size), stack_(stack) {}

size_t StackOverlayMemory::Read(uint64_t addr, void* dst, size_t size) {
  if (addr >= sp_ && addr + size <= stack_end_ && addr + size > sp_) {
    size_t offset = static_cast<size_t>(addr - sp_);
    memcpy(dst, stack_ + offset, size);
    return size;
  }

  return mem_->Read(addr, dst, size);
}

FDMemory::FDMemory(base::ScopedFile mem_fd) : mem_fd_(std::move(mem_fd)) {}

size_t FDMemory::Read(uint64_t addr, void* dst, size_t size) {
  ssize_t rd = pread64(*mem_fd_, dst, size, static_cast<off64_t>(addr));
  if (rd == -1) {
    PERFETTO_DPLOG("read of %zu at offset %" PRIu64, size, addr);
    return 0;
  }
  return static_cast<size_t>(rd);
}

FDMaps::FDMaps(base::ScopedFile fd) : fd_(std::move(fd)) {}

bool FDMaps::Parse() {
  // If the process has already exited, lseek or ReadFileDescriptor will
  // return false.
  if (lseek(*fd_, 0, SEEK_SET) == -1)
    return false;

  std::string content;
  if (!base::ReadFileDescriptor(*fd_, &content))
    return false;

  unwindstack::MapInfo* prev_map = nullptr;
  unwindstack::MapInfo* prev_real_map = nullptr;
  return android::procinfo::ReadMapFileContent(
      &content[0], [&](uint64_t start, uint64_t end, uint16_t flags,
                       uint64_t pgoff, ino_t, const char* name) {
        // Mark a device map in /dev/ and not in /dev/ashmem/ specially.
        if (strncmp(name, "/dev/", 5) == 0 &&
            strncmp(name + 5, "ashmem/", 7) != 0) {
          flags |= unwindstack::MAPS_FLAGS_DEVICE_MAP;
        }
        maps_.emplace_back(new unwindstack::MapInfo(
            prev_map, prev_real_map, start, end, pgoff, flags, name));
        prev_map = maps_.back().get();
        if (!prev_map->IsBlank()) {
          prev_real_map = prev_map;
        }
      });
}

void FDMaps::Reset() {
  maps_.clear();
}

}  // namespace profiling
}  // namespace perfetto
