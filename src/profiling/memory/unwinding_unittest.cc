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

#include "src/profiling/memory/unwinding.h"
#include "perfetto/base/scoped_file.h"
#include "src/profiling/memory/transport_data.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include <cxxabi.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <unwindstack/RegsGetLocal.h>

namespace perfetto {
namespace {

TEST(UnwindingTest, StackMemoryOverlay) {
  base::ScopedFile proc_mem(open("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_mem);
  uint8_t fake_stack[1] = {120};
  StackMemory memory(*proc_mem, 0u, fake_stack, 1);
  uint8_t buf[1] = {};
  ASSERT_EQ(memory.Read(0u, buf, 1), 1);
  ASSERT_EQ(buf[0], 120);
}

TEST(UnwindingTest, StackMemoryNonOverlay) {
  uint8_t value = 52;

  base::ScopedFile proc_mem(open("/proc/self/mem", O_RDONLY));
  ASSERT_TRUE(proc_mem);
  uint8_t fake_stack[1] = {120};
  StackMemory memory(*proc_mem, 0u, fake_stack, 1);
  uint8_t buf[1] = {1};
  ASSERT_EQ(memory.Read(reinterpret_cast<uint64_t>(&value), buf, 1), 1);
  ASSERT_EQ(buf[0], value);
}

TEST(UnwindingTest, FileDescriptorMapsParse) {
  base::ScopedFile proc_maps(open("/proc/self/maps", O_RDONLY));
  ASSERT_TRUE(proc_maps);
  FileDescriptorMaps maps(std::move(proc_maps));
  ASSERT_TRUE(maps.Parse());
  unwindstack::MapInfo* map_info =
      maps.Find(reinterpret_cast<uint64_t>(&proc_maps));
  ASSERT_NE(map_info, nullptr);
  ASSERT_EQ(map_info->name, "[stack]");
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#define MAYBE_DoUnwind DoUnwind
#else
#define MAYBE_DoUnwind DISABLED_DoUnwind
#endif

uint8_t* GetStackBase() {
  pthread_t t = pthread_self();
  pthread_attr_t attr;
  if (pthread_getattr_np(t, &attr) != 0) {
    return nullptr;
  }
  uint8_t* x;
  size_t s;
  if (pthread_attr_getstack(&attr, reinterpret_cast<void**>(&x), &s) != 0)
    return nullptr;
  pthread_attr_destroy(&attr);
  return x + s;
}

// This is needed because ASAN thinks copying the whole stack is a buffer
// underrun.
void __attribute__((noinline))
UnsafeMemcpy(void* dst, const void* src, size_t n)
    __attribute__((no_sanitize("address"))) {
  const uint8_t* from = reinterpret_cast<const uint8_t*>(src);
  uint8_t* to = reinterpret_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; ++i)
    to[i] = from[i];
}

std::pair<std::unique_ptr<uint8_t[]>, size_t> __attribute__((noinline))
GetRecord() {
  const uint8_t* stackbase = GetStackBase();
  PERFETTO_CHECK(stackbase != nullptr);
  const uint8_t* stacktop =
      reinterpret_cast<uint8_t*>(__builtin_frame_address(0));
  PERFETTO_CHECK(stacktop != nullptr);
  PERFETTO_CHECK(stacktop < stackbase);
  const size_t stack_size = static_cast<size_t>(stackbase - stacktop);
  std::unique_ptr<unwindstack::Regs> regs(unwindstack::Regs::CreateFromLocal());
  const unwindstack::ArchEnum arch = regs->CurrentArch();
  const size_t reg_size = RegSize(arch);
  const size_t total_size = sizeof(AllocMetadata) + reg_size + stack_size;
  std::unique_ptr<uint8_t[]> buf(new uint8_t[total_size]);
  AllocMetadata* metadata = reinterpret_cast<AllocMetadata*>(buf.get());
  metadata->alloc_size = 0;
  metadata->alloc_address = 0;
  metadata->stack_pointer = reinterpret_cast<uint64_t>(stacktop);
  metadata->stack_pointer_offset = sizeof(AllocMetadata) + reg_size;
  metadata->arch = arch;
  unwindstack::RegsGetLocal(regs.get());
  // Make sure nothing above has changed the stack pointer, just for extra
  // paranoia.
  PERFETTO_CHECK(stacktop ==
                 reinterpret_cast<uint8_t*>(__builtin_frame_address(0)));
  memcpy(buf.get() + sizeof(AllocMetadata), regs->RawData(), reg_size);
  UnsafeMemcpy(buf.get() + sizeof(AllocMetadata) + reg_size, stacktop,
               stack_size);
  return {std::move(buf), total_size};
}

// TODO(fmayer): Investigate why this fails out of tree.
TEST(UnwindingTest, MAYBE_DoUnwind) {
  base::ScopedFile proc_maps(open("/proc/self/maps", O_RDONLY));
  base::ScopedFile proc_mem(open("/proc/self/mem", O_RDONLY));
  ProcessMetadata metadata(getpid(), std::move(proc_maps), std::move(proc_mem));
  auto record = GetRecord();
  std::vector<unwindstack::FrameData> out;
  ASSERT_TRUE(DoUnwind(record.first.get(), record.second, &metadata, &out));
  int st;
  std::unique_ptr<char> demangled(
      abi::__cxa_demangle(out[0].function_name.c_str(), nullptr, nullptr, &st));
  ASSERT_EQ(st, 0);
  ASSERT_STREQ(demangled.get(), "perfetto::(anonymous namespace)::GetRecord()");
}

}  // namespace
}  // namespace perfetto
