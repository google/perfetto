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

#ifndef SRC_PROFILING_MEMORY_SHARED_RING_BUFFER_H_
#define SRC_PROFILING_MEMORY_SHARED_RING_BUFFER_H_

#include "perfetto/base/optional.h"
#include "perfetto/base/unix_socket.h"
#include "perfetto/base/utils.h"

#include <atomic>
#include <map>
#include <memory>

#include <stdint.h>

namespace perfetto {
namespace profiling {

class ScopedSpinlock {
 public:
  enum class Mode { Try, Blocking };

  ScopedSpinlock(std::atomic<bool>* lock, Mode mode) : lock_(lock) {
    if (PERFETTO_LIKELY(!lock_->exchange(true, std::memory_order_acquire))) {
      locked_ = true;
      return;
    }
    LockSlow(mode);
  }

  ScopedSpinlock(const ScopedSpinlock&) = delete;
  ScopedSpinlock& operator=(const ScopedSpinlock&) = delete;

  ScopedSpinlock(ScopedSpinlock&&) noexcept;
  ScopedSpinlock& operator=(ScopedSpinlock&&);

  ~ScopedSpinlock();

  void Unlock() {
    if (locked_) {
      PERFETTO_DCHECK(lock_->load());
      lock_->store(false, std::memory_order_release);
    }
    locked_ = false;
  }

  bool locked() const { return locked_; }

 private:
  void LockSlow(Mode mode);
  std::atomic<bool>* lock_;
  bool locked_ = false;
};

// A concurrent, multi-writer single-reader ring buffer FIFO, based on a
// circular buffer over shared memory. It has similar semantics to a SEQ_PACKET
// + O_NONBLOCK socket, specifically:
//
// - Writes are atomic, data is either written fully in the buffer or not.
// - New writes are discarded if the buffer is full.
// - If a write succeeds, the reader is guaranteed to see the whole buffer.
// - Reads are atomic, no fragmentation.
// - The reader sees writes in write order (% discarding).
//
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
// *IMPORTANT*: The ring buffer must be written under the assumption that the
// other end modifies arbitrary shared memory without holding the spin-lock.
// This means we must make local copies of read and write pointers for doing
// bounds checks followed by reads / writes, as they might change in the
// meantime.
// !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
//
// TODO:
// - Write a benchmark.
// - Make the stats ifdef-able.
class SharedRingBuffer {
 public:
  class Buffer {
   public:
    Buffer() {}
    Buffer(uint8_t* d, size_t s) : data(d), size(s) {}

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&&) = default;
    Buffer& operator=(Buffer&&) = default;

    operator bool() const { return data != nullptr; }

    uint8_t* data = nullptr;
    size_t size = 0;
  };

  static base::Optional<SharedRingBuffer> Create(size_t);
  static base::Optional<SharedRingBuffer> Attach(base::ScopedFile);

  ~SharedRingBuffer();
  SharedRingBuffer(SharedRingBuffer&&) noexcept;
  SharedRingBuffer& operator=(SharedRingBuffer&&);

  bool is_valid() const { return !!mem_; }
  size_t size() const { return size_; }
  int fd() const { return *mem_fd_; }

  Buffer BeginWrite(const ScopedSpinlock& spinlock, size_t size);
  void EndWrite(Buffer buf);

  Buffer BeginRead();
  void EndRead(Buffer);

  // This is used by the caller to be able to hold the SpinLock after
  // BeginWrite has returned. This is so that additional bookkeeping can be
  // done under the lock. This will be used to increment the sequence_number.
  ScopedSpinlock AcquireLock(ScopedSpinlock::Mode mode) {
    return ScopedSpinlock(&meta_->spinlock, mode);
  }

 private:
  struct alignas(base::kPageSize) MetadataPage {
    alignas(uint64_t) std::atomic<bool> spinlock;
    uint64_t read_pos;
    uint64_t write_pos;

    // stats, for debugging only.
    std::atomic<uint64_t> failed_spinlocks;
    std::atomic<uint64_t> bytes_written;
    std::atomic<uint64_t> num_writes_succeeded;
    std::atomic<uint64_t> num_writes_failed;
    std::atomic<uint64_t> num_reads_failed;
  };

  struct PointerPositions {
    uint64_t read_pos;
    uint64_t write_pos;
  };

  struct CreateFlag {};
  struct AttachFlag {};
  SharedRingBuffer(const SharedRingBuffer&) = delete;
  SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
  SharedRingBuffer(CreateFlag, size_t size);
  SharedRingBuffer(AttachFlag, base::ScopedFile mem_fd) {
    Initialize(std::move(mem_fd));
  }

  void Initialize(base::ScopedFile mem_fd);
  bool IsCorrupt(const PointerPositions& pos);

  inline base::Optional<PointerPositions> GetPointerPositions(
      const ScopedSpinlock& lock) {
    PERFETTO_DCHECK(lock.locked());

    PointerPositions pos;
    pos.read_pos = meta_->read_pos;
    pos.write_pos = meta_->write_pos;

    base::Optional<PointerPositions> result;
    if (IsCorrupt(pos)) {
      meta_->num_reads_failed++;
      return result;
    }
    result = pos;
    return result;
  }

  inline size_t read_avail(const PointerPositions& pos) {
    PERFETTO_DCHECK(pos.write_pos >= pos.read_pos);
    auto res = static_cast<size_t>(pos.write_pos - pos.read_pos);
    PERFETTO_DCHECK(res <= size_);
    return res;
  }

  inline size_t write_avail(const PointerPositions& pos) {
    return size_ - read_avail(pos);
  }

  inline uint8_t* at(uint64_t pos) { return mem_ + (pos & (size_ - 1)); }

  base::ScopedFile mem_fd_;
  MetadataPage* meta_ = nullptr;  // Start of the mmaped region.
  uint8_t* mem_ = nullptr;  // Start of the contents (i.e. meta_ + kPageSize).

  // Size of the ring buffer contents, without including metadata or the 2nd
  // mmap.
  size_t size_ = 0;

  // Remember to update the move ctor when adding new fields.
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_SHARED_RING_BUFFER_H_
