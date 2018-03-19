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
#include <sys/stat.h>
#include <unistd.h>

#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "perfetto/base/scoped_file.h"
#include "perfetto/base/temp_file.h"
#include "perfetto/base/utils.h"
#include "src/base/test/test_task_runner.h"
#include "src/base/test/vm_test_utils.h"

namespace perfetto {
namespace {

bool IsFileDescriptorClosed(int fd) {
  return lseek(fd, 0, SEEK_CUR) == -1 && errno == EBADF;
}

TEST(PosixSharedMemoryTest, DestructorUnmapsMemory) {
  PosixSharedMemory::Factory factory;
  std::unique_ptr<SharedMemory> shm =
      factory.CreateSharedMemory(base::kPageSize);
  void* const shm_start = shm->start();
  const size_t shm_size = shm->size();
  ASSERT_NE(nullptr, shm_start);
  ASSERT_EQ(base::kPageSize, shm_size);

  memcpy(shm_start, "test", 5);
  ASSERT_TRUE(base::vm_test_utils::IsMapped(shm_start, shm_size));

  shm.reset();
  ASSERT_FALSE(base::vm_test_utils::IsMapped(shm_start, shm_size));
}

TEST(PosixSharedMemoryTest, DestructorClosesFD) {
  std::unique_ptr<PosixSharedMemory> shm =
      PosixSharedMemory::Create(base::kPageSize);
  int fd = shm->fd();
  ASSERT_GE(fd, 0);
  ASSERT_EQ(static_cast<off_t>(base::kPageSize), lseek(fd, 0, SEEK_END));

  shm.reset();
  ASSERT_TRUE(IsFileDescriptorClosed(fd));
}

TEST(PosixSharedMemoryTest, AttachToFd) {
  base::TempFile tmp_file = base::TempFile::CreateUnlinked();
  const int fd_num = tmp_file.fd();
  ASSERT_EQ(0, ftruncate(fd_num, base::kPageSize));
  ASSERT_EQ(7, PERFETTO_EINTR(write(fd_num, "foobar", 7)));

  std::unique_ptr<PosixSharedMemory> shm =
      PosixSharedMemory::AttachToFd(tmp_file.ReleaseFD());
  void* const shm_start = shm->start();
  const size_t shm_size = shm->size();
  ASSERT_NE(nullptr, shm_start);
  ASSERT_EQ(base::kPageSize, shm_size);
  ASSERT_EQ(0, memcmp("foobar", shm_start, 7));

  ASSERT_FALSE(IsFileDescriptorClosed(fd_num));

  shm.reset();
  ASSERT_TRUE(IsFileDescriptorClosed(fd_num));
  ASSERT_FALSE(base::vm_test_utils::IsMapped(shm_start, shm_size));
}

}  // namespace
}  // namespace perfetto
