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

namespace perfetto {

// Types needed for the wire format used for communication between the client
// and heapprofd. The basic format of a record is
// record size (uint64_t) | record type (RecordType = uint64_t) | record
// If record type is malloc, the record format is AllocMetdata | raw stack.
// If the record type is free, the record is a sequence of FreePageEntry.

// Use uint64_t to make sure the following data is aligned as 64bit is the
// strongest alignment requirement.

// C++11 std::max is not constexpr.
constexpr size_t constexpr_max(size_t x, size_t y) {
  return x > y ? x : y;
}

constexpr size_t kMaxRegisterDataSize = constexpr_max(
    constexpr_max(constexpr_max(unwindstack::ARM_REG_LAST * sizeof(uint32_t),
                                unwindstack::ARM64_REG_LAST * sizeof(uint64_t)),
                  unwindstack::X86_REG_LAST * sizeof(uint32_t)),
    unwindstack::X86_64_REG_LAST * sizeof(uint64_t));

constexpr size_t kFreePageSize = 1024;

enum class RecordType : uint64_t {
  Free = 0,
  Malloc = 1,
};

struct AllocMetadata {
  uint64_t sequence_number;
  // Size of the allocation that was made.
  uint64_t alloc_size;
  // Pointer returned by malloc(2) for this allocation.
  uint64_t alloc_address;
  // Current value of the stack pointer.
  uint64_t stack_pointer;
  // Offset of the data at stack_pointer from the start of this record.
  uint64_t stack_pointer_offset;
  // CPU architecture of the client. This determines the size of the
  // register data that follows this struct.
  unwindstack::ArchEnum arch;
  char register_data[kMaxRegisterDataSize];
};

struct FreePageEntry {
  uint64_t sequence_number;
  uint64_t addr;
};

struct FreeMetadata {
  uint64_t num_entries;
  FreePageEntry entries[kFreePageSize];
};

struct WireMessage {
  RecordType record_type;

  AllocMetadata* alloc_header;
  FreeMetadata* free_header;

  char* payload;
  size_t payload_size;
};

bool SendWireMessage(int sock, const WireMessage& msg);

// Parse message received over the wire.
// |buf| has to outlive |out|.
// If buf is not a valid message, return false.
bool ReceiveWireMessage(char* buf, size_t size, WireMessage* out);

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_WIRE_PROTOCOL_H_
