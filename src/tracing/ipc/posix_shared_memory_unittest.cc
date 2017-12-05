/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "src/tracing/ipc/posix_shared_memory.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/utils.h"
#include "src/base/test/test_task_runner.h"

namespace perfetto {
namespace {

const size_t kPageSize = 4096;

bool IsFileDescriptorClosed(int fd) {
  return lseek(fd, 0, SEEK_CUR) == -1 && errno == EBADF;
}

bool IsMapped(void* start, size_t size) {
#if BUILDFLAG(OS_MACOSX)
  using PageState = char;
#else
  using PageState = unsigned char;
#endif
  EXPECT_EQ(0u, size % kPageSize);
  const size_t num_pages = size / kPageSize;
  std::unique_ptr<PageState[]> page_states(new PageState[num_pages]);
  memset(page_states.get(), 0, num_pages * sizeof(PageState));
  int res = mincore(start, size, page_states.get());
  // Linux returns ENOMEM when an unmapped memory range is passed.
  // MacOS instead returns 0 but leaves the page_states empty.
  if (res == -1 && errno == ENOMEM)
    return false;
  EXPECT_EQ(0, res);
  for (size_t i = 0; i < num_pages; i++) {
    if (!page_states[i])
      return false;
  }
  return true;
}

TEST(PosixSharedMemoryTest, DestructorUnmapsMemory) {
  PosixSharedMemory::Factory factory;
  std::unique_ptr<SharedMemory> shm = factory.CreateSharedMemory(kPageSize);
  void* const shm_start = shm->start();
  const size_t shm_size = shm->size();
  ASSERT_NE(nullptr, shm_start);
  ASSERT_EQ(kPageSize, shm_size);

  memcpy(shm_start, "test", 5);
  ASSERT_TRUE(IsMapped(shm_start, shm_size));

  shm.reset();
  ASSERT_FALSE(IsMapped(shm_start, shm_size));
}

TEST(PosixSharedMemoryTest, DestructorClosesFD) {
  std::unique_ptr<PosixSharedMemory> shm = PosixSharedMemory::Create(kPageSize);
  int fd = shm->fd();
  ASSERT_GE(fd, 0);
  ASSERT_EQ(static_cast<off_t>(kPageSize), lseek(fd, 0, SEEK_END));

  shm.reset();
  ASSERT_TRUE(IsFileDescriptorClosed(fd));
}

TEST(PosixSharedMemoryTest, AttachToFd) {
  FILE* tmp_file = tmpfile();  // Creates an unlinked auto-deleting temp file.
  const int fd_num = fileno(tmp_file);
  ASSERT_EQ(0, ftruncate(fd_num, kPageSize));
  ASSERT_EQ(7, PERFETTO_EINTR(write(fd_num, "foobar", 7)));

  std::unique_ptr<PosixSharedMemory> shm =
      PosixSharedMemory::AttachToFd(base::ScopedFile(fd_num));
  void* const shm_start = shm->start();
  const size_t shm_size = shm->size();
  ASSERT_NE(nullptr, shm_start);
  ASSERT_EQ(kPageSize, shm_size);
  ASSERT_EQ(0, memcmp("foobar", shm_start, 7));

  ASSERT_FALSE(IsFileDescriptorClosed(fd_num));

  shm.reset();
  ASSERT_TRUE(IsFileDescriptorClosed(fd_num));
  ASSERT_FALSE(IsMapped(shm_start, shm_size));
}

}  // namespace
}  // namespace perfetto
