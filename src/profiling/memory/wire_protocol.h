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

// The data types used for communication between heapprofd and the client
// embedded in processes that are being profiled.

#ifndef SRC_PROFILING_MEMORY_WIRE_PROTOCOL_H_
#define SRC_PROFILING_MEMORY_WIRE_PROTOCOL_H_

#include <inttypes.h>
#include <unwindstack/Elf.h>
#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/MachineMips.h>
#include <unwindstack/MachineMips64.h>
#include <unwindstack/MachineX86.h>
#include <unwindstack/MachineX86_64.h>

#include "perfetto/profiling/memory/client_ext.h"
#include "src/profiling/memory/shared_ring_buffer.h"

// Make sure the alignment is the same on 32 and 64 bit architectures. This
// is to ensure the structs below are laid out in exactly the same way for
// both of those, at the same build.
// The maximum alignment of every type T is sizeof(T), so we overalign that.
// E.g., the alignment for uint64_t is 4 bytes on 32, and 8 bytes on 64 bit.
#define PERFETTO_CROSS_ABI_ALIGNED(type) alignas(sizeof(type)) type

namespace perfetto {

namespace base {
class UnixSocketRaw;
}

namespace profiling {

struct ClientConfiguration {
  // On average, sample one allocation every interval bytes,
  // If interval == 1, sample every allocation.
  // Must be >= 1.
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) interval;
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) block_client_timeout_us;
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) num_heaps;
  PERFETTO_CROSS_ABI_ALIGNED(char) heaps[64][HEAPPROFD_HEAP_NAME_SZ];
  PERFETTO_CROSS_ABI_ALIGNED(bool) block_client;
  PERFETTO_CROSS_ABI_ALIGNED(bool) disable_fork_teardown;
  PERFETTO_CROSS_ABI_ALIGNED(bool) disable_vfork_detection;
  PERFETTO_CROSS_ABI_ALIGNED(bool) all_heaps;
  // Just double check that the array sizes are in correct order.
  static_assert(sizeof(heaps[0]) == HEAPPROFD_HEAP_NAME_SZ, "");
};

// Types needed for the wire format used for communication between the client
// and heapprofd. The basic format of a record is
// record size (uint64_t) | record type (RecordType = uint64_t) | record
// If record type is malloc, the record format is AllocMetdata | raw stack.
// If the record type is free, the record is a sequence of FreeBatchEntry.

// Use uint64_t to make sure the following data is aligned as 64bit is the
// strongest alignment requirement.

// C++11 std::max is not constexpr.
constexpr size_t constexpr_max(size_t x, size_t y) {
  return x > y ? x : y;
}

// clang-format makes this unreadable. Turning it off for this block.
// clang-format off
constexpr size_t kMaxRegisterDataSize =
  constexpr_max(
    constexpr_max(
      constexpr_max(
        constexpr_max(
            constexpr_max(
              sizeof(uint32_t) * unwindstack::ARM_REG_LAST,
              sizeof(uint64_t) * unwindstack::ARM64_REG_LAST),
            sizeof(uint32_t) * unwindstack::X86_REG_LAST),
          sizeof(uint64_t) * unwindstack::X86_64_REG_LAST),
        sizeof(uint32_t) * unwindstack::MIPS_REG_LAST),
      sizeof(uint64_t) * unwindstack::MIPS64_REG_LAST
  );
// clang-format on

enum class RecordType : uint64_t {
  Free = 0,
  Malloc = 1,
  HeapName = 2,
};

// Make the whole struct 8-aligned. This is to make sizeof(AllocMetdata)
// the same on 32 and 64-bit.
struct alignas(8) AllocMetadata {
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) sequence_number;
  // Size of the allocation that was made.
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) alloc_size;
  // Total number of bytes attributed to this allocation.
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) sample_size;
  // Pointer returned by malloc(2) for this allocation.
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) alloc_address;
  // Current value of the stack pointer.
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) stack_pointer;
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) clock_monotonic_coarse_timestamp;
  // unwindstack::AsmGetRegs assumes this is aligned.
  alignas(8) char register_data[kMaxRegisterDataSize];
  PERFETTO_CROSS_ABI_ALIGNED(uint32_t) heap_id;
  // CPU architecture of the client.
  PERFETTO_CROSS_ABI_ALIGNED(unwindstack::ArchEnum) arch;
};

struct FreeEntry {
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) sequence_number;
  PERFETTO_CROSS_ABI_ALIGNED(uint64_t) addr;
  PERFETTO_CROSS_ABI_ALIGNED(uint32_t) heap_id;
};

struct HeapName {
  PERFETTO_CROSS_ABI_ALIGNED(uint32_t) heap_id;
  PERFETTO_CROSS_ABI_ALIGNED(char) heap_name[HEAPPROFD_HEAP_NAME_SZ];
};

static_assert(sizeof(AllocMetadata) == 328,
              "AllocMetadata needs to be the same size across ABIs.");
static_assert(sizeof(FreeEntry) == 24,
              "FreeEntry needs to be the same size across ABIs.");
static_assert(sizeof(HeapName) == 68,
              "HeapName needs to be the same size across ABIs.");
static_assert(sizeof(ClientConfiguration) == 4128,
              "ClientConfiguration needs to be the same size across ABIs.");

enum HandshakeFDs : size_t {
  kHandshakeMaps = 0,
  kHandshakeMem,
  kHandshakePageIdle,
  kHandshakeSize,
};

struct WireMessage {
  RecordType record_type;

  AllocMetadata* alloc_header;
  FreeEntry* free_header;
  HeapName* heap_name_header;

  char* payload;
  size_t payload_size;
};

int64_t SendWireMessage(SharedRingBuffer* buf, const WireMessage& msg);

// Parse message received over the wire.
// |buf| has to outlive |out|.
// If buf is not a valid message, return false.
bool ReceiveWireMessage(char* buf, size_t size, WireMessage* out);

constexpr const char* kHeapprofdSocketEnvVar = "ANDROID_SOCKET_heapprofd";
constexpr const char* kHeapprofdSocketFile = "/dev/socket/heapprofd";

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_WIRE_PROTOCOL_H_
