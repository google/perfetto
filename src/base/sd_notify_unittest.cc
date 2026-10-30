/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "perfetto/ext/base/sd_notify.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)

#include <stdlib.h>
#include <unistd.h>

#include <string>

#include "perfetto/ext/base/unix_socket.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace base {
namespace {

class SdNotifyTest : public ::testing::Test {
 protected:
  void SetUp() override { unsetenv("NOTIFY_SOCKET"); }
  void TearDown() override { unsetenv("NOTIFY_SOCKET"); }

  // Binds a datagram server on an abstract address and points NOTIFY_SOCKET at
  // it.
  UnixSocketRaw BindServer() {
    std::string name = "@perfetto_sd_notify_test_" + std::to_string(getpid());
    auto server =
        UnixSocketRaw::CreateMayFail(SockFamily::kUnix, SockType::kDgram);
    EXPECT_TRUE(server);
    EXPECT_TRUE(server.Bind(name));
    setenv("NOTIFY_SOCKET", name.c_str(), /*overwrite=*/1);
    return server;
  }
};

TEST_F(SdNotifyTest, UnsetIsNoop) {
  EXPECT_FALSE(SdNotify("READY=1"));
}

TEST_F(SdNotifyTest, EmptyIsNoop) {
  setenv("NOTIFY_SOCKET", "", /*overwrite=*/1);
  EXPECT_FALSE(SdNotify("READY=1"));
}

TEST_F(SdNotifyTest, SendsToAbstractSocket) {
  auto server = BindServer();
  ASSERT_TRUE(SdNotify("READY=1"));
  char buf[32] = {};
  ssize_t rsize = server.Receive(buf, sizeof(buf));
  ASSERT_EQ(rsize, 7);
  EXPECT_EQ(std::string(buf, static_cast<size_t>(rsize)), "READY=1");
}

TEST_F(SdNotifyTest, UnsupportedAddress) {
  setenv("NOTIFY_SOCKET", "relative/path", /*overwrite=*/1);
  EXPECT_FALSE(SdNotify("READY=1"));
}

TEST_F(SdNotifyTest, NobodyListening) {
  setenv("NOTIFY_SOCKET", "@perfetto_sd_notify_test_nobody", /*overwrite=*/1);
  EXPECT_FALSE(SdNotify("READY=1"));
}

}  // namespace
}  // namespace base
}  // namespace perfetto

#endif  // PERFETTO_OS_LINUX || PERFETTO_OS_ANDROID
