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

#ifndef SRC_PROFILING_MEMORY_TRANSPORT_DATA_H_
#define SRC_PROFILING_MEMORY_TRANSPORT_DATA_H_

#include <inttypes.h>
#include <unwindstack/Elf.h>

namespace perfetto {

// Use uint64_t to make sure the following data is aligned as 64bit is the
// strongest alignment requirement.
enum class RecordType : uint64_t {
  Free = 0,
  Malloc = 1,
};

struct AllocMetadata {
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
};

}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_TRANSPORT_DATA_H_
