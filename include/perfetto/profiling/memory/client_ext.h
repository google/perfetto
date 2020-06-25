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

#ifndef INCLUDE_PERFETTO_PROFILING_MEMORY_CLIENT_EXT_H_
#define INCLUDE_PERFETTO_PROFILING_MEMORY_CLIENT_EXT_H_

#include <inttypes.h>
#include <stdlib.h>

#define HEAPPROFD_HEAP_NAME_SZ 32

#ifdef __cplusplus
extern "C" {
#endif

// This struct is append only. Be very careful that the ABI of this does not
// change. We want to be able to correctly handle structs from clients that
// compile against old versions of this header, setting all the newly added
// fields to zero.
//
// TODO(fmayer): Sort out alignment etc. before stabilizing the ABI.
struct HeapprofdHeapInfo {
  char heap_name[HEAPPROFD_HEAP_NAME_SZ];
  // Gets called when heap profiling gets enabled or disabled.
  void (*callback)(bool /* enabled */);
};

bool heapprofd_init_session(void* (*malloc_fn)(size_t), void (*free_fn)(void*));

uint32_t heapprofd_register_heap(const HeapprofdHeapInfo* heap_info, size_t n);

bool heapprofd_report_allocation(uint32_t heap_id, uint64_t id, uint64_t size);

void heapprofd_report_free(uint32_t heap_id, uint64_t id);

#ifdef __cplusplus
}
#endif

#endif  // INCLUDE_PERFETTO_PROFILING_MEMORY_CLIENT_EXT_H_
