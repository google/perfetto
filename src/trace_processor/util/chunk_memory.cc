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

#include "src/trace_processor/util/chunk_memory.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/temp_file.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#define TP_CHUNK_MEMORY_CAN_PAGE 1
#include <unistd.h>
#endif

namespace perfetto::trace_processor {

ChunkMemory::ChunkMemory(size_t chunk_size, size_t resident_budget)
    : chunk_size_(chunk_size), resident_budget_(resident_budget) {}

ChunkMemory::~ChunkMemory() = default;

namespace {

#if defined(TP_CHUNK_MEMORY_CAN_PAGE)

// pread and pwrite with a 64-bit offset, which off_t is not on 32-bit Android.
ssize_t PreadAt(int fd, uint8_t* data, size_t size, uint64_t offset) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return pread64(fd, data, size, static_cast<off64_t>(offset));
#else
  PERFETTO_CHECK(offset <=
                 static_cast<uint64_t>(std::numeric_limits<off_t>::max()));
  return pread(fd, data, size, static_cast<off_t>(offset));
#endif
}

ssize_t PwriteAt(int fd, const uint8_t* data, size_t size, uint64_t offset) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return pwrite64(fd, data, size, static_cast<off64_t>(offset));
#else
  PERFETTO_CHECK(offset <=
                 static_cast<uint64_t>(std::numeric_limits<off_t>::max()));
  return pwrite(fd, data, size, static_cast<off_t>(offset));
#endif
}

// Heap chunks which can be paged out to an unlinked temporary file with plain
// reads and writes. Slots are chunk sized and reused, so the file never
// grows past the peak paged out. The page cache, not this process, holds what
// is paged out until the kernel writes it back.
class PagedChunkMemory : public ChunkMemory {
 public:
  PagedChunkMemory(base::ScopedFile fd, size_t chunk_size, size_t budget)
      : ChunkMemory(chunk_size, budget), fd_(std::move(fd)) {}

  uint32_t PageOut(const uint8_t* data, size_t size) override {
    uint32_t slot;
    if (!free_slots_.empty()) {
      slot = free_slots_.back();
      free_slots_.pop_back();
    } else {
      slot = slot_count_++;
    }
    PERFETTO_DCHECK(size <= chunk_size());
    uint64_t offset = SlotOffset(slot);
    while (size > 0) {
      ssize_t written = PERFETTO_EINTR(PwriteAt(*fd_, data, size, offset));
      PERFETTO_CHECK(written > 0);
      data += written;
      size -= static_cast<size_t>(written);
      offset += static_cast<uint64_t>(written);
    }
    return slot;
  }

  void PageIn(uint32_t slot, uint8_t* data, size_t size) override {
    uint64_t offset = SlotOffset(slot);
    while (size > 0) {
      ssize_t read = PERFETTO_EINTR(PreadAt(*fd_, data, size, offset));
      PERFETTO_CHECK(read > 0);
      data += read;
      size -= static_cast<size_t>(read);
      offset += static_cast<uint64_t>(read);
    }
    free_slots_.push_back(slot);
  }

  void FreePage(uint32_t slot) override { free_slots_.push_back(slot); }

 private:
  uint64_t SlotOffset(uint32_t slot) const {
    return uint64_t{slot} * uint64_t{chunk_size()};
  }

  base::ScopedFile fd_;
  uint32_t slot_count_ = 0;
  std::vector<uint32_t> free_slots_;
};

// What stays resident while the rest is paged out. A larger budget holds more
// of the sorter's pending events in memory; a smaller one pages more chunks
// out and back in, each with one write and one read of a chunk.
constexpr size_t kNativeResidentBudget = 4 * 1024 * 1024;

#endif  // defined(TP_CHUNK_MEMORY_CAN_PAGE)

}  // namespace

uint32_t ChunkMemory::PageOut(const uint8_t*, size_t) {
  PERFETTO_FATAL("Chunks cannot be paged out");
}

void ChunkMemory::PageIn(uint32_t, uint8_t*, size_t) {
  PERFETTO_FATAL("Chunks cannot be paged out");
}

void ChunkMemory::FreePage(uint32_t) {
  PERFETTO_FATAL("Chunks cannot be paged out");
}

// static
RefPtr<ChunkMemory> ChunkMemory::Create(IoMode mode, size_t chunk_size) {
#if defined(TP_CHUNK_MEMORY_CAN_PAGE)
  if (mode == IoMode::kNative) {
    if (RefPtr<ChunkMemory> paged = Paged(chunk_size, kNativeResidentBudget)) {
      return paged;
    }
    PERFETTO_ELOG("Cannot create a temporary file; chunks stay resident");
  }
#else
  base::ignore_result(mode);
#endif
  return Heap(chunk_size);
}

// static
RefPtr<ChunkMemory> ChunkMemory::Paged(size_t chunk_size,
                                       size_t resident_budget) {
#if defined(TP_CHUNK_MEMORY_CAN_PAGE)
  base::StatusOr<base::TempFile> file =
      base::TempFile::Create(base::TempFile::LinkMode::kUnlinked);
  if (file.ok()) {
    return RefPtr<ChunkMemory>(
        new PagedChunkMemory(file->ReleaseFD(), chunk_size, resident_budget));
  }
#else
  base::ignore_result(chunk_size, resident_budget);
#endif
  return RefPtr<ChunkMemory>();
}

// static
RefPtr<ChunkMemory> ChunkMemory::Heap(size_t chunk_size) {
  return RefPtr<ChunkMemory>(new ChunkMemory(chunk_size, kNoBudget));
}

}  // namespace perfetto::trace_processor
