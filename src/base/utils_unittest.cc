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

#include "perfetto/ext/base/utils.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <stdint.h>

#include <algorithm>
#include <random>
#include <thread>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/pipe.h"
#include "perfetto/ext/base/temp_file.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

TEST(UtilsTest, ArraySize) {
  char char_arr_1[1];
  char char_arr_4[4];
  EXPECT_EQ(1u, ArraySize(char_arr_1));
  EXPECT_EQ(4u, ArraySize(char_arr_4));

  int32_t int32_arr_1[1];
  int32_t int32_arr_4[4];
  EXPECT_EQ(1u, ArraySize(int32_arr_1));
  EXPECT_EQ(4u, ArraySize(int32_arr_4));

  uint64_t int64_arr_1[1];
  uint64_t int64_arr_4[4];
  EXPECT_EQ(1u, ArraySize(int64_arr_1));
  EXPECT_EQ(4u, ArraySize(int64_arr_4));

  char kString[] = "foo";
  EXPECT_EQ(4u, ArraySize(kString));

  struct Bar {
    int32_t a;
    int32_t b;
  };
  Bar bar_1[1];
  Bar bar_4[4];
  EXPECT_EQ(1u, ArraySize(bar_1));
  EXPECT_EQ(4u, ArraySize(bar_4));
}

TEST(UtilsTest, PipeBlockingRW) {
  Pipe pipe = Pipe::Create();
  std::string expected;
  expected.resize(1024 * 512u);
  for (size_t i = 0; i < expected.size(); i++)
    expected[i] = '!' + static_cast<char>(i % 64);

  std::thread writer([&] {
    std::string tx = expected;
    std::minstd_rand0 rnd_engine(0);

    while (!tx.empty()) {
      size_t wsize = static_cast<size_t>(rnd_engine() % 4096) + 1;
      wsize = std::min(wsize, tx.size());
      WriteAllHandle(*pipe.wr, &tx[0], wsize);
      tx.erase(0, wsize);
    }
    pipe.wr.reset();
  });

  std::string actual;
  ASSERT_TRUE(ReadPlatformHandle(*pipe.rd, &actual));
  ASSERT_EQ(actual, expected);
  writer.join();
}

// Tests that WriteAllHandle and ReadPlatformHandle work as advertised.
// TODO(primiano): normalize File handling on Windows. Right now some places
// use POSIX-compat APIs that use "int" file descriptors (_open, _read, _write),
// some other places use WINAPI files (CreateFile(), ReadFile()), where the file
// is a HANDLE.
TEST(UtilsTest, ReadWritePlatformHandle) {
  auto tmp = TempDir::Create();
  std::string payload = "foo\nbar\0baz\r\nqux";
  std::string tmp_path = tmp.path() + "/temp.txt";

  // Write a file using PlatformHandle. Note: the {} blocks are to make sure
  // that the file is automatically closed via RAII before being reopened.
  {
    ScopedPlatformHandle handle {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
      ::CreateFileA(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr)
#else
      OpenFile(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif
    };
    ASSERT_TRUE(handle);
    ASSERT_EQ(WriteAllHandle(*handle, payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
  }

  // Read it back.
  {
    ScopedPlatformHandle handle {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
      ::CreateFileA(tmp_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr)
#else
      OpenFile(tmp_path, O_RDONLY)
#endif
    };
    ASSERT_TRUE(handle);
    std::string actual = "preserve_existing:";
    ASSERT_TRUE(ReadPlatformHandle(*handle, &actual));
    ASSERT_EQ(actual, "preserve_existing:" + payload);
  }

  ASSERT_EQ(remove(tmp_path.c_str()), 0);
}

// Fuchsia doesn't currently support sigaction(), see
// https://fxbug.dev/30518 .
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
TEST(UtilsTest, EintrWrapper) {
  Pipe pipe = Pipe::Create();

  struct sigaction sa = {};
  struct sigaction old_sa = {};

// Glibc headers for sa_sigaction trigger this.
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wdisabled-macro-expansion"
#endif
  sa.sa_sigaction = [](int, siginfo_t*, void*) {};
#pragma GCC diagnostic pop

  ASSERT_EQ(0, sigaction(SIGUSR2, &sa, &old_sa));
  int parent_pid = getpid();
  pid_t pid = fork();
  ASSERT_NE(-1, pid);
  if (pid == 0 /* child */) {
    usleep(5000);
    kill(parent_pid, SIGUSR2);
    ignore_result(WriteAll(*pipe.wr, "foo\0", 4));
    _exit(0);
  }

  char buf[6] = {};
  EXPECT_EQ(4, PERFETTO_EINTR(read(*pipe.rd, buf, sizeof(buf))));
  EXPECT_TRUE(close(*pipe.rd) == 0 || errno == EINTR);
  pipe.wr.reset();

  // A 2nd close should fail with the proper errno.
  int res = close(*pipe.rd);
  auto err = errno;
  EXPECT_EQ(-1, res);
  EXPECT_EQ(EBADF, err);
  pipe.rd.release();

  // Restore the old handler.
  sigaction(SIGUSR2, &old_sa, nullptr);
}
#endif  // LINUX | ANDROID | APPLE

TEST(UtilsTest, Align) {
  EXPECT_EQ(0u, AlignUp<4>(0));
  EXPECT_EQ(4u, AlignUp<4>(1));
  EXPECT_EQ(4u, AlignUp<4>(3));
  EXPECT_EQ(4u, AlignUp<4>(4));
  EXPECT_EQ(8u, AlignUp<4>(5));
  EXPECT_EQ(0u, AlignUp<16>(0));
  EXPECT_EQ(16u, AlignUp<16>(1));
  EXPECT_EQ(16u, AlignUp<16>(15));
  EXPECT_EQ(16u, AlignUp<16>(16));
  EXPECT_EQ(32u, AlignUp<16>(17));
  EXPECT_EQ(0xffffff00u, AlignUp<16>(0xffffff00 - 1));
}

TEST(UtilsTest, HexDump) {
  char input[] = {0x00, 0x00, 'a', 'b', 'c', 'd', 'e', 'f', 'g',
                  'h',  'i',  'j', 'k', 'l', 'm', 'n', 'o', 'p'};

  std::string output = HexDump(input, sizeof(input));

  EXPECT_EQ(
      output,
      R"(00000000: 00 00 61 62 63 64 65 66 67 68 69 6A 6B 6C 6D 6E   ..abcdefghijklmn
00000010: 6F 70                                             op
)");
}

}  // namespace
}  // namespace base
}  // namespace perfetto
