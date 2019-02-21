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

#include "src/profiling/memory/shared_ring_buffer.h"

#include <atomic>
#include <type_traits>

#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "perfetto/base/build_config.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/temp_file.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace perfetto {
namespace profiling {

namespace {

constexpr auto kMetaPageSize = base::kPageSize;
constexpr auto kAlignment = 8;  // 64 bits to use aligned memcpy().
constexpr auto kHeaderSize = kAlignment;
constexpr auto kGuardSize = base::kPageSize * 1024 * 16;  // 64 MB.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
constexpr auto kFDSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
#endif

}  // namespace

void ScopedSpinlock::LockSlow(Mode mode) {
  // Slowpath.
  for (size_t attempt = 0; mode == Mode::Blocking || attempt < 1024 * 10;
       attempt++) {
    if (!lock_->load(std::memory_order_relaxed) &&
        PERFETTO_LIKELY(!lock_->exchange(true, std::memory_order_acquire))) {
      locked_ = true;
      return;
    }
    if (attempt && attempt % 1024 == 0)
      usleep(1000);
  }
}

ScopedSpinlock::ScopedSpinlock(ScopedSpinlock&& other) noexcept
    : lock_(other.lock_), locked_(other.locked_) {
  other.locked_ = false;
}

ScopedSpinlock& ScopedSpinlock::operator=(ScopedSpinlock&& other) {
  if (this != &other) {
    this->~ScopedSpinlock();
    new (this) ScopedSpinlock(std::move(other));
  }
  return *this;
}

ScopedSpinlock::~ScopedSpinlock() {
  Unlock();
}

SharedRingBuffer::SharedRingBuffer(CreateFlag, size_t size) {
  size_t size_with_meta = size + kMetaPageSize;
  // TODO(primiano): this is copy/pasted from posix_shared_memory.cc . Refactor.
  base::ScopedFile fd;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  bool is_memfd = false;
  fd.reset(static_cast<int>(syscall(__NR_memfd_create, "heaprofd_ringbuf",
                                    MFD_CLOEXEC | MFD_ALLOW_SEALING)));
  is_memfd = !!fd;

  if (!fd) {
    // TODO: if this fails on Android we should fall back on ashmem.
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    // In-tree builds should only allow mem_fd, so we can inspect the seals
    // to verify the fd is appropriately sealed.
    PERFETTO_ELOG("memfd_create() failed");
    return;
#else
    PERFETTO_DPLOG("memfd_create() failed");
#endif
  }
#endif

  if (!fd)
    fd = base::TempFile::CreateUnlinked().ReleaseFD();

  PERFETTO_CHECK(fd);
  int res = ftruncate(fd.get(), static_cast<off_t>(size_with_meta));
  PERFETTO_CHECK(res == 0);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  if (is_memfd) {
    res = fcntl(*fd, F_ADD_SEALS, kFDSeals);
    PERFETTO_DCHECK(res == 0);
  }
#endif
  Initialize(std::move(fd));
  new (meta_) MetadataPage();
}

SharedRingBuffer::~SharedRingBuffer() {
  static_assert(std::is_trivially_constructible<MetadataPage>::value,
                "MetadataPage must be trivially constructible");
  static_assert(std::is_trivially_destructible<MetadataPage>::value,
                "MetadataPage must be trivially destructible");

  if (is_valid()) {
    size_t outer_size = kMetaPageSize + size_ * 2 + kGuardSize;
    munmap(meta_, outer_size);
  }
}

void SharedRingBuffer::Initialize(base::ScopedFile mem_fd) {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  int seals = fcntl(*mem_fd, F_GET_SEALS);
  if ((seals & kFDSeals) != kFDSeals) {
    PERFETTO_ELOG("FD not properly sealed. Expected %x, got %x", kFDSeals,
                  seals);
    return;
  }
#endif

  struct stat stat_buf = {};
  int res = fstat(*mem_fd, &stat_buf);
  if (res != 0 || stat_buf.st_size == 0) {
    PERFETTO_PLOG("Could not attach to fd.");
    return;
  }
  auto size_with_meta = static_cast<size_t>(stat_buf.st_size);
  auto size = size_with_meta - kMetaPageSize;

  // |size_with_meta| must be a power of two number of pages + 1 page (for
  // metadata).
  if (size_with_meta < 2 * base::kPageSize || size % base::kPageSize ||
      (size & (size - 1))) {
    PERFETTO_ELOG("SharedRingBuffer size is invalid (%zu)", size_with_meta);
    return;
  }

  // First of all reserve the whole virtual region to fit the buffer twice
  // + metadata page + red zone at the end.
  size_t outer_size = kMetaPageSize + size * 2 + kGuardSize;
  uint8_t* region = reinterpret_cast<uint8_t*>(
      mmap(nullptr, outer_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (region == MAP_FAILED) {
    PERFETTO_PLOG("mmap(PROT_NONE) failed");
    return;
  }

  // Map first the whole buffer (including the initial metadata page) @ off=0.
  void* reg1 = mmap(region, size_with_meta, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, *mem_fd, 0);

  // Then map again the buffer, skipping the metadata page. The final result is:
  // [ METADATA ] [ RING BUFFER SHMEM ] [ RING BUFFER SHMEM ]
  void* reg2 = mmap(region + size_with_meta, size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_FIXED, *mem_fd,
                    /*offset=*/kMetaPageSize);

  if (reg1 != region || reg2 != region + size_with_meta) {
    PERFETTO_PLOG("mmap(MAP_SHARED) failed");
    munmap(region, outer_size);
    return;
  }
  size_ = size;
  meta_ = reinterpret_cast<MetadataPage*>(region);
  mem_ = region + kMetaPageSize;
  mem_fd_ = std::move(mem_fd);
}

SharedRingBuffer::Buffer SharedRingBuffer::BeginWrite(
    const ScopedSpinlock& spinlock,
    size_t size) {
  PERFETTO_DCHECK(spinlock.locked());
  Buffer result;

  base::Optional<PointerPositions> opt_pos = GetPointerPositions(spinlock);
  if (!opt_pos)
    return result;
  auto pos = opt_pos.value();

  const uint64_t size_with_header =
      base::AlignUp<kAlignment>(size + kHeaderSize);
  if (size_with_header > write_avail(pos)) {
    meta_->num_writes_failed++;
    return result;
  }

  uint8_t* wr_ptr = at(pos.write_pos);

  result.size = size;
  result.data = wr_ptr + kHeaderSize;
  meta_->write_pos += size_with_header;
  meta_->bytes_written += size;
  meta_->num_writes_succeeded++;
  // By making this a release store, we can save grabbing the spinlock in
  // EndWrite.
  reinterpret_cast<std::atomic<uint32_t>*>(wr_ptr)->store(
      0, std::memory_order_release);
  return result;
}

void SharedRingBuffer::EndWrite(Buffer buf) {
  uint8_t* wr_ptr = buf.data - kHeaderSize;
  PERFETTO_DCHECK(reinterpret_cast<uintptr_t>(wr_ptr) % kAlignment == 0);
  reinterpret_cast<std::atomic<uint32_t>*>(wr_ptr)->store(
      static_cast<uint32_t>(buf.size), std::memory_order_release);
}

SharedRingBuffer::Buffer SharedRingBuffer::BeginRead() {
  ScopedSpinlock spinlock(&meta_->spinlock, ScopedSpinlock::Mode::Blocking);

  base::Optional<PointerPositions> opt_pos = GetPointerPositions(spinlock);
  if (!opt_pos)
    return Buffer();
  auto pos = opt_pos.value();

  size_t avail_read = read_avail(pos);

  if (avail_read < kHeaderSize)
    return Buffer();  // No data

  uint8_t* rd_ptr = at(pos.read_pos);
  PERFETTO_DCHECK(reinterpret_cast<uintptr_t>(rd_ptr) % kAlignment == 0);
  const size_t size = reinterpret_cast<std::atomic<uint32_t>*>(rd_ptr)->load(
      std::memory_order_acquire);
  if (size == 0)
    return Buffer();
  const size_t size_with_header = base::AlignUp<kAlignment>(size + kHeaderSize);

  if (size_with_header > avail_read) {
    PERFETTO_ELOG(
        "Corrupted header detected, size=%zu"
        ", read_avail=%zu, rd=%" PRIu64 ", wr=%" PRIu64,
        size, avail_read, pos.read_pos, pos.write_pos);
    meta_->num_reads_failed++;
    return Buffer();
  }

  rd_ptr += kHeaderSize;
  PERFETTO_DCHECK(reinterpret_cast<uintptr_t>(rd_ptr) % kAlignment == 0);
  return Buffer(rd_ptr, size);
}

void SharedRingBuffer::EndRead(Buffer buf) {
  if (!buf)
    return;
  ScopedSpinlock spinlock(&meta_->spinlock, ScopedSpinlock::Mode::Blocking);
  size_t size_with_header = base::AlignUp<kAlignment>(buf.size + kHeaderSize);
  meta_->read_pos += size_with_header;
}

bool SharedRingBuffer::IsCorrupt(const PointerPositions& pos) {
  if (pos.write_pos < pos.read_pos || pos.write_pos - pos.read_pos > size_ ||
      pos.write_pos % kAlignment || pos.read_pos % kAlignment) {
    PERFETTO_ELOG("Ring buffer corrupted, rd=%" PRIu64 ", wr=%" PRIu64
                  ", size=%zu",
                  pos.read_pos, pos.write_pos, size_);
    return true;
  }
  return false;
}

SharedRingBuffer::SharedRingBuffer(SharedRingBuffer&& other) noexcept {
  *this = std::move(other);
}

SharedRingBuffer& SharedRingBuffer::operator=(SharedRingBuffer&& other) {
  mem_fd_ = std::move(other.mem_fd_);
  std::tie(meta_, mem_, size_) = std::tie(other.meta_, other.mem_, other.size_);
  std::tie(other.meta_, other.mem_, other.size_) =
      std::make_tuple(nullptr, nullptr, 0);
  return *this;
}

// static
base::Optional<SharedRingBuffer> SharedRingBuffer::Create(size_t size) {
  auto buf = SharedRingBuffer(CreateFlag(), size);
  if (!buf.is_valid())
    return base::nullopt;
  return base::make_optional(std::move(buf));
}

// static
base::Optional<SharedRingBuffer> SharedRingBuffer::Attach(
    base::ScopedFile mem_fd) {
  auto buf = SharedRingBuffer(AttachFlag(), std::move(mem_fd));
  if (!buf.is_valid())
    return base::nullopt;
  return base::make_optional(std::move(buf));
}

}  // namespace profiling
}  // namespace perfetto
