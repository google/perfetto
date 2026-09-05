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

#ifndef SRC_TRACING_V2_SHARED_RING_BUFFER_TEST_UTILS_H_
#define SRC_TRACING_V2_SHARED_RING_BUFFER_TEST_UTILS_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <string>

#include "perfetto/ext/base/no_destructor.h"
#include "perfetto/ext/base/paged_memory.h"
#include "src/tracing/v2/shared_ring_buffer.h"
#include "src/tracing/v2/shared_ring_buffer_abi.h"
#include "src/tracing/v2/shared_ring_buffer_reader.h"
#include "src/tracing/v2/shared_ring_buffer_writer.h"

namespace perfetto::tracing_v2::test {

// Owns zero-filled, page-aligned memory for a ring and exposes the non-owning
// SharedRingBuffer view over it. Production code allocates the region
// elsewhere. Tests use this so that the ring layout is specified only once.
class SharedRingBufferForTesting {
 public:
  SharedRingBufferForTesting(uint32_t num_chunks, uint32_t chunk_size)
      : memory_(base::PagedMemory::Allocate(RingSize(num_chunks, chunk_size))),
        ring_(static_cast<uint8_t*>(memory_.Get()),
              RingSize(num_chunks, chunk_size),
              chunk_size) {}

  SharedRingBuffer* get() { return &ring_; }
  SharedRingBuffer* operator->() { return &ring_; }

 private:
  static size_t RingSize(uint32_t num_chunks, uint32_t chunk_size) {
    return sizeof(RingBufferHeader) +
           static_cast<size_t>(num_chunks) * chunk_size;
  }

  base::PagedMemory memory_;
  SharedRingBuffer ring_;
};

// Reaches the private state that the deterministic tests need. Friended by
// SharedRingBuffer and SharedRingBufferReader.
class SharedRingBufferInternalsForTest {
 public:
  // Injects a state word for corruption and unknown-ABI tests.
  static void SetChunkStateWord(SharedRingBuffer* ring,
                                uint32_t chunk_idx,
                                uint32_t state_word) {
    ring->chunk_state_word_at(chunk_idx)->store(state_word,
                                                std::memory_order_release);
  }

  // Injects a write_pos without reserving the intervening positions.
  static void SetWritePos(SharedRingBuffer* ring, uint32_t write_pos) {
    RingBufferHeader* header = ring->header();
    const uint64_t rw_positions =
        header->rw_positions.load(std::memory_order_relaxed);
    header->rw_positions.store(
        PackRwPositions(write_pos, ReadPosOf(rw_positions)),
        std::memory_order_relaxed);
  }

  // Seeds a valid ring state near a position or wrap-count rollover: every
  // chunk becomes Free for the first position at or after |position| that maps
  // to it, and both positions are set to |position|.
  static void SetPositions(SharedRingBuffer* ring, uint32_t position) {
    for (uint32_t chunk_idx = 0; chunk_idx < ring->num_chunks(); ++chunk_idx) {
      const uint32_t first_position =
          position + ((chunk_idx - position) & (ring->num_chunks() - 1));
      ring->chunk_state_word_at(chunk_idx)->store(
          MakeFreeStateWord(
              WrapCountForPosition(first_position, ring->num_chunks())),
          std::memory_order_relaxed);
    }
    ring->header()->rw_positions.store(PackRwPositions(position, position),
                                       std::memory_order_release);
  }

  static uint32_t GetReadPos(const SharedRingBuffer* ring) {
    return ReadPosOf(
        ring->header()->rw_positions.load(std::memory_order_relaxed));
  }

  static uint32_t GetNumWritersWaiting(const SharedRingBuffer* ring) {
    return ring->header()->num_writers_waiting.load(std::memory_order_relaxed);
  }

  // Start the production CAS loops from an old rw_positions snapshot, so that
  // the first compare-and-swap fails deterministically.
  static SharedRingBuffer::Reservation TryReserveWritePosFromSnapshot(
      SharedRingBuffer* ring,
      uint64_t rw_positions) {
    return ring->TryReserveWritePosFromSnapshot(rw_positions);
  }
  static void PublishReadPosFromSnapshot(SharedRingBuffer* ring,
                                         uint64_t rw_positions,
                                         uint32_t read_pos) {
    ring->PublishReadPosFromSnapshot(rw_positions, read_pos);
  }

  // Starts the reader at |position| instead of zero, to match a ring seeded
  // near uint32_t rollover.
  static void SetReaderPos(SharedRingBufferReader* reader, uint32_t position) {
    reader->read_pos_ = position;
  }

  static SharedRingBufferReader::ResolveResult ResolveNextPosition(
      SharedRingBufferReader* reader) {
    return reader->ResolveNextPosition();
  }
};

class NoopSharedRingBufferWriterDelegate
    : public SharedRingBufferWriter::Delegate {
 public:
  void NotifyReader() override {}
};

inline SharedRingBufferWriter::Delegate*
GetNoopSharedRingBufferWriterDelegate() {
  static base::NoDestructor<NoopSharedRingBufferWriterDelegate> delegate;
  return &delegate.ref();
}

// Decodes the wrap count of a Free word, which lives in the WriterID field.
// Production compares whole Free words and never needs this.
constexpr uint16_t WrapCountOf(uint32_t state_word) {
  return static_cast<uint16_t>(WriterIDOf(state_word));
}

// Writes one fragment holding |bytes| and publishes it. Returns false if the
// fragment could not be begun or if publishing it dropped a relocated suffix.
inline bool WriteFragment(SharedRingBufferWriter* writer,
                          const std::string& bytes,
                          bool continues_from_prev = false,
                          bool continues_on_next = false) {
  const SharedRingBufferWriter::FragmentRange range = writer->BeginFragment(
      static_cast<uint32_t>(bytes.size()), continues_from_prev);
  if (range.result != SharedRingBufferWriter::BeginFragmentResult::kSuccess)
    return false;
  memcpy(range.begin, bytes.data(), bytes.size());
  return writer->EndFragment(static_cast<uint32_t>(bytes.size()),
                             continues_on_next) ==
         SharedRingBufferWriter::EndFragmentResult::kSuccess;
}

}  // namespace perfetto::tracing_v2::test

#endif  // SRC_TRACING_V2_SHARED_RING_BUFFER_TEST_UTILS_H_
