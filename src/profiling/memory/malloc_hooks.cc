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

#include <bionic/malloc.h>
#include <inttypes.h>
#include <malloc.h>
#include <private/bionic_malloc_dispatch.h>

#include <atomic>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/profiling/memory/client_ext.h"

// This is so we can make an so that we can swap out with the existing
// libc_malloc_hooks.so
#ifndef HEAPPROFD_PREFIX
#define HEAPPROFD_PREFIX heapprofd
#endif

#define HEAPPROFD_ADD_PREFIX(name) \
  PERFETTO_BUILDFLAG_CAT(HEAPPROFD_PREFIX, name)

#pragma GCC visibility push(default)
extern "C" {

bool HEAPPROFD_ADD_PREFIX(_initialize)(const MallocDispatch* malloc_dispatch,
                                       bool* zygote_child,
                                       const char* options);
void HEAPPROFD_ADD_PREFIX(_finalize)();
void HEAPPROFD_ADD_PREFIX(_dump_heap)(const char* file_name);
void HEAPPROFD_ADD_PREFIX(_get_malloc_leak_info)(uint8_t** info,
                                                 size_t* overall_size,
                                                 size_t* info_size,
                                                 size_t* total_memory,
                                                 size_t* backtrace_size);
bool HEAPPROFD_ADD_PREFIX(_write_malloc_leak_info)(FILE* fp);
ssize_t HEAPPROFD_ADD_PREFIX(_malloc_backtrace)(void* pointer,
                                                uintptr_t* frames,
                                                size_t frame_count);
void HEAPPROFD_ADD_PREFIX(_free_malloc_leak_info)(uint8_t* info);
size_t HEAPPROFD_ADD_PREFIX(_malloc_usable_size)(void* pointer);
void* HEAPPROFD_ADD_PREFIX(_malloc)(size_t size);
void HEAPPROFD_ADD_PREFIX(_free)(void* pointer);
void* HEAPPROFD_ADD_PREFIX(_aligned_alloc)(size_t alignment, size_t size);
void* HEAPPROFD_ADD_PREFIX(_memalign)(size_t alignment, size_t bytes);
void* HEAPPROFD_ADD_PREFIX(_realloc)(void* pointer, size_t bytes);
void* HEAPPROFD_ADD_PREFIX(_calloc)(size_t nmemb, size_t bytes);
struct mallinfo HEAPPROFD_ADD_PREFIX(_mallinfo)();
int HEAPPROFD_ADD_PREFIX(_mallopt)(int param, int value);
int HEAPPROFD_ADD_PREFIX(_malloc_info)(int options, FILE* fp);
int HEAPPROFD_ADD_PREFIX(_posix_memalign)(void** memptr,
                                          size_t alignment,
                                          size_t size);
int HEAPPROFD_ADD_PREFIX(_malloc_iterate)(uintptr_t base,
                                          size_t size,
                                          void (*callback)(uintptr_t base,
                                                           size_t size,
                                                           void* arg),
                                          void* arg);
void HEAPPROFD_ADD_PREFIX(_malloc_disable)();
void HEAPPROFD_ADD_PREFIX(_malloc_enable)();

#if defined(HAVE_DEPRECATED_MALLOC_FUNCS)
void* HEAPPROFD_ADD_PREFIX(_pvalloc)(size_t bytes);
void* HEAPPROFD_ADD_PREFIX(_valloc)(size_t size);
#endif
}
#pragma GCC visibility pop

namespace {

// The real malloc function pointers we get in initialize. Set once in the first
// initialize invocation, and never changed afterwards. Because bionic does a
// release write after initialization and an acquire read to retrieve the hooked
// malloc functions, we can use relaxed memory mode for both writing and
// reading.
std::atomic<const MallocDispatch*> g_dispatch{nullptr};

const MallocDispatch* GetDispatch() {
  return g_dispatch.load(std::memory_order_relaxed);
}

// Note: android_mallopt(M_RESET_HOOKS) is mutually exclusive with
// heapprofd_initialize. Concurrent calls get discarded, which might be our
// unpatching attempt if there is a concurrent re-initialization running due to
// a new signal.
void ProfileCallback(bool enabled) {
  if (!enabled) {
    if (!android_mallopt(M_RESET_HOOKS, nullptr, 0))
      PERFETTO_PLOG("Unpatching heapprofd hooks failed.");
  }
}

uint32_t g_heap_id = AHeapProfile_registerHeap(
    AHeapInfo_setCallback(AHeapInfo_create("com.android.malloc"),
                          ProfileCallback));

}  // namespace

// Setup for the rest of profiling. The first time profiling is triggered in a
// process, this is called after this client library is dlopened, but before the
// rest of the hooks are patched in. However, as we support multiple profiling
// sessions within a process' lifetime, this function can also be legitimately
// called any number of times afterwards (note: bionic guarantees that at most
// one initialize call is active at a time).
//
// Note: if profiling is triggered at runtime, this runs on a dedicated pthread
// (which is safe to block). If profiling is triggered at startup, then this
// code runs synchronously.
bool HEAPPROFD_ADD_PREFIX(_initialize)(const MallocDispatch* malloc_dispatch,
                                       bool*,
                                       const char*) {
  // Table of pointers to backing implementation.
  g_dispatch.store(malloc_dispatch);
  return AHeapProfile_initSession(malloc_dispatch->malloc,
                                  malloc_dispatch->free);
}

void HEAPPROFD_ADD_PREFIX(_finalize)() {
  // At the time of writing, invoked only as an atexit handler. We don't have
  // any specific action to take, and cleanup can be left to the OS.
}

void* HEAPPROFD_ADD_PREFIX(_malloc)(size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  void* addr = dispatch->malloc(size);
  AHeapProfile_reportAllocation(g_heap_id, reinterpret_cast<uint64_t>(addr),
                                size);
  return addr;
}

void* HEAPPROFD_ADD_PREFIX(_calloc)(size_t nmemb, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  void* addr = dispatch->calloc(nmemb, size);
  AHeapProfile_reportAllocation(g_heap_id, reinterpret_cast<uint64_t>(addr),
                                nmemb * size);
  return addr;
}

void* HEAPPROFD_ADD_PREFIX(_aligned_alloc)(size_t alignment, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  void* addr = dispatch->aligned_alloc(alignment, size);
  AHeapProfile_reportAllocation(g_heap_id, reinterpret_cast<uint64_t>(addr),
                                size);
  return addr;
}

void* HEAPPROFD_ADD_PREFIX(_memalign)(size_t alignment, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  void* addr = dispatch->memalign(alignment, size);
  AHeapProfile_reportAllocation(g_heap_id, reinterpret_cast<uint64_t>(addr),
                                size);
  return addr;
}

int HEAPPROFD_ADD_PREFIX(_posix_memalign)(void** memptr,
                                          size_t alignment,
                                          size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  int res = dispatch->posix_memalign(memptr, alignment, size);
  if (res != 0)
    return res;

  AHeapProfile_reportAllocation(g_heap_id, reinterpret_cast<uint64_t>(*memptr),
                                size);
  return 0;
}

// Note: we record the free before calling the backing implementation to make
// sure that the address is not reused before we've processed the deallocation
// (which includes assigning a sequence id to it).
void HEAPPROFD_ADD_PREFIX(_free)(void* pointer) {
  // free on a nullptr is valid but has no effect. Short circuit here, for
  // various advantages:
  // * More efficient
  // * Notably printf calls free(nullptr) even when it is used in a way
  //   malloc-free way, as it unconditionally frees the pointer even if
  //   it was never written to.
  //   Short circuiting here makes it less likely to accidentally build
  //   infinite recursion.
  if (pointer == nullptr)
    return;

  const MallocDispatch* dispatch = GetDispatch();
  AHeapProfile_reportFree(g_heap_id, reinterpret_cast<uint64_t>(pointer));
  return dispatch->free(pointer);
}

// Approach to recording realloc: under the initial lock, get a safe copy of the
// client, and make the sampling decision in advance. Then record the
// deallocation, call the real realloc, and finally record the sample if one is
// necessary.
//
// As with the free, we record the deallocation before calling the backing
// implementation to make sure the address is still exclusive while we're
// processing it.
void* HEAPPROFD_ADD_PREFIX(_realloc)(void* pointer, size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  if (pointer)
    AHeapProfile_reportFree(g_heap_id, reinterpret_cast<uint64_t>(pointer));
  void* addr = dispatch->realloc(pointer, size);
  AHeapProfile_reportAllocation(g_heap_id, reinterpret_cast<uint64_t>(addr),
                                size);
  return addr;
}

void HEAPPROFD_ADD_PREFIX(_dump_heap)(const char*) {}

void HEAPPROFD_ADD_PREFIX(
    _get_malloc_leak_info)(uint8_t**, size_t*, size_t*, size_t*, size_t*) {}

bool HEAPPROFD_ADD_PREFIX(_write_malloc_leak_info)(FILE*) {
  return false;
}

ssize_t HEAPPROFD_ADD_PREFIX(_malloc_backtrace)(void*, uintptr_t*, size_t) {
  return -1;
}

void HEAPPROFD_ADD_PREFIX(_free_malloc_leak_info)(uint8_t*) {}

size_t HEAPPROFD_ADD_PREFIX(_malloc_usable_size)(void* pointer) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_usable_size(pointer);
}

struct mallinfo HEAPPROFD_ADD_PREFIX(_mallinfo)() {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->mallinfo();
}

int HEAPPROFD_ADD_PREFIX(_mallopt)(int param, int value) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->mallopt(param, value);
}

int HEAPPROFD_ADD_PREFIX(_malloc_info)(int options, FILE* fp) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_info(options, fp);
}

int HEAPPROFD_ADD_PREFIX(_malloc_iterate)(uintptr_t base,
                                          size_t size,
                                          void (*callback)(uintptr_t base,
                                                   size_t size,
                                                   void* arg),
                                          void* arg) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_iterate(base, size, callback, arg);
}

void HEAPPROFD_ADD_PREFIX(_malloc_disable)() {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_disable();
}

void HEAPPROFD_ADD_PREFIX(_malloc_enable)() {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->malloc_enable();
}

#if defined(HAVE_DEPRECATED_MALLOC_FUNCS)
void* HEAPPROFD_ADD_PREFIX(_pvalloc)(size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->pvalloc(size);
}

void* HEAPPROFD_ADD_PREFIX(_valloc)(size_t size) {
  const MallocDispatch* dispatch = GetDispatch();
  return dispatch->valloc(size);
}

#endif
