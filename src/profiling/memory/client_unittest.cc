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

#include "src/profiling/memory/client.h"

#include "gtest/gtest.h"

#include <thread>

namespace perfetto {
namespace {

TEST(SocketPoolTest, Basic) {
  std::vector<base::ScopedFile> files;
  files.emplace_back(open("/dev/null", O_RDONLY));
  SocketPool pool(std::move(files));
  BorrowedSocket sock = pool.Borrow();
}

TEST(SocketPoolTest, Multiple) {
  std::vector<base::ScopedFile> files;
  files.emplace_back(open("/dev/null", O_RDONLY));
  files.emplace_back(open("/dev/null", O_RDONLY));
  SocketPool pool(std::move(files));
  BorrowedSocket sock = pool.Borrow();
  BorrowedSocket sock_2 = pool.Borrow();
}

TEST(SocketPoolTest, Blocked) {
  std::vector<base::ScopedFile> files;
  files.emplace_back(open("/dev/null", O_RDONLY));
  SocketPool pool(std::move(files));
  BorrowedSocket sock = pool.Borrow();
  std::thread t([&pool] { pool.Borrow(); });
  {
    // Return fd to unblock thread.
    BorrowedSocket temp = std::move(sock);
  }
  t.join();
}

TEST(SocketPoolTest, MultipleBlocked) {
  std::vector<base::ScopedFile> files;
  files.emplace_back(open("/dev/null", O_RDONLY));
  SocketPool pool(std::move(files));
  BorrowedSocket sock = pool.Borrow();
  std::thread t([&pool] { pool.Borrow(); });
  std::thread t2([&pool] { pool.Borrow(); });
  {
    // Return fd to unblock thread.
    BorrowedSocket temp = std::move(sock);
  }
  t.join();
  t2.join();
}

}  // namespace
}  // namespace perfetto
