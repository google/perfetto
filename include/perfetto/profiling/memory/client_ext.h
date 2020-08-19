/*
 * Copyright (C) 2020 The Android Open Source Project
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

// API to report allocations to heapprofd. This allows users to see the
// callstacks causing these allocations in heap profiles.
//
// In the context of this API, a "heap" is memory associated with an allocator.
// An example for allocator is the malloc-family of libc functions (malloc /
// calloc / posix_memalign).
//
// A very simple custom allocator would look like this:
//
// void* my_malloc(size_t size) {
//   void* ptr = [code to somehow allocate get size bytes];
//   return ptr;
// }
//
// void my_free(void* ptr) {
//   [code to somehow free ptr]
// }
//
// To find out where in a program these two functions get called, we instrument
// the allocator using this API:
//
// static HeapprofdHeapInfo g_info{"invalid.example", nullptr};
// static uint32_t g_heap_id = heapprofd_register_heap(&g_info, sizeof(g_info));
// void* my_malloc(size_t size) {
//   void* ptr = [code to somehow allocate get size bytes];
//   heapprofd_report_allocation(g_heap_id, static_cast<uintptr_t>(ptr), size);
//   return ptr;
// }
//
// void my_free(void* ptr) {
//   heapprofd_report_free(g_heap_id, static_cast<uintptr_t>(ptr));
//   [code to somehow free ptr]
// }
//
// This will allow users to get a flamegraph of the callstacks calling into
// these functions.
//
// See https://perfetto.dev/docs/data-sources/native-heap-profiler for more
// information on heapprofd in general.

#ifndef INCLUDE_PERFETTO_PROFILING_MEMORY_CLIENT_EXT_H_
#define INCLUDE_PERFETTO_PROFILING_MEMORY_CLIENT_EXT_H_

#include <inttypes.h>
#include <stdlib.h>

// Maximum size of heap name, including NUL-byte.
#define HEAPPROFD_HEAP_NAME_SZ 64

#ifdef __cplusplus
extern "C" {
#endif

// Metadata of a custom heap.
//
// heapprofd maintainers NB: This struct is append only. Be very careful that
// the ABI of this does not change. We want to be able to correctly handle
// structs from clients that compile against old versions of this header,
// setting all the newly added fields to zero.
//
// TODO(fmayer): Sort out alignment etc. before stabilizing the ABI.
struct HeapprofdHeapInfo {
  // Name of the heap, up to 64 bytes including NUL-terminator. To guarantee
  // uniqueness, this should include the caller's domain name, e.g.
  // "com.android.malloc".
  // This member MUST be set.
  alignas(8) char heap_name[HEAPPROFD_HEAP_NAME_SZ];

  // Gets called when heap profiling gets enabled or disabled. Can be NULL if
  // no function should get called.
  void (*callback)(bool /* enabled */);
};

typedef struct HeapprofdHeapInfo HeapprofdHeapInfo;

#ifdef __cplusplus
static_assert(alignof(HeapprofdHeapInfo) == 8,
              "HeapprofdHeapInfo must be aligned to 64bit.");
#endif

// Called by libc upon receipt of the profiling signal.
// DO NOT CALL EXCEPT FROM LIBC!
// TODO(fmayer): Maybe move this out of this header.
bool heapprofd_init_session(void* (*malloc_fn)(size_t), void (*free_fn)(void*));

// Register a heap. Options are given in the HeapprofdHeapInfo struct.
//
// On success, returns a heap_id that is used in heapprofd_report_allocation
// and heapprofd_report_free to report operations on this heap.
//
// On error, returns 0, which can be safely passed to any function expecting a
// |heap_id|, and will turn them into a no-op.
//
// This is safe to call from a static initializer.
uint32_t heapprofd_register_heap(const HeapprofdHeapInfo* heap_info,
                                 size_t sizeof_heap_info);

// Reports an allocation of |size| on the given |heap_id|.
//
// If a profiling session is active, this function decides whether the reported
// allocation should be sampled. If the allocation is sampled, it will be
// associated to the current callstack in the profile.
//
// Returns whether the allocation was sampled.
bool heapprofd_report_allocation(uint32_t heap_id,
                                 uint64_t alloc_id,
                                 uint64_t size);

// Report allocation was freed on the given heap.
//
// If |alloc_id| was sampled in a previous call to heapprofd_report_allocation,
// this allocation is marked as freed in the profile.
//
// It is allowed to call with an |alloc_id| that was either not sampled or never
// passed to heapprofd_report_allocation, in which case the call will not
// change the output.
void heapprofd_report_free(uint32_t heap_id, uint64_t alloc_id);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PERFETTO_PROFILING_MEMORY_CLIENT_EXT_H_
