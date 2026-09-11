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

#include "perfetto/ext/base/progress_reporter.h"

#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/utils.h"
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)
#include <sys/ioctl.h>
#include <unistd.h>
#endif
#include "test/gtest_and_gmock.h"

namespace perfetto::base {
namespace {
class ProgressReporterTest : public testing::Test {
 protected:
  void SetUp() override {
    for (const char* name : {"TERM", "NO_COLOR", "FORCE_COLOR"}) {
      const char* value = getenv(name);
      saved_[name] = value ? std::optional<std::string>(value) : std::nullopt;
      UnsetEnv(name);
    }
    testing::internal::CaptureStderr();
  }
  void TearDown() override {
    if (!captured_)
      testing::internal::GetCapturedStderr();
    for (const auto& item : saved_) {
      if (item.second)
        SetEnv(item.first, *item.second);
      else
        UnsetEnv(item.first);
    }
  }
  std::string Output() {
    captured_ = true;
    return testing::internal::GetCapturedStderr();
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)
  std::string OnTerminal(const std::function<void()>& action) {
    auto master = OpenFile("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
    PERFETTO_CHECK(master);
    PERFETTO_CHECK(grantpt(*master) == 0);
    PERFETTO_CHECK(unlockpt(*master) == 0);
    auto slave = OpenFile(ptsname(*master), O_RDWR | O_NOCTTY);
    PERFETTO_CHECK(slave);
    struct winsize size{};
    size.ws_col = 20;
    PERFETTO_CHECK(ioctl(*slave, TIOCSWINSZ, &size) == 0);
    auto saved_stderr = DupFile(STDERR_FILENO);
    PERFETTO_CHECK(saved_stderr);
    fflush(stderr);
    PERFETTO_CHECK(dup2(*slave, STDERR_FILENO) == STDERR_FILENO);
    action();
    fflush(stderr);
    PERFETTO_CHECK(dup2(*saved_stderr, STDERR_FILENO) == STDERR_FILENO);
    std::string output;
    char buffer[1024];
    for (;;) {
      ssize_t size_read = read(*master, buffer, sizeof(buffer));
      if (size_read <= 0)
        break;
      output.append(buffer, static_cast<size_t>(size_read));
    }
    return output;
  }
#endif

 private:
  bool captured_ = false;
  std::map<std::string, std::optional<std::string>> saved_;
};

TEST_F(ProgressReporterTest, RedirectedOutputHasNoProgressOrColor) {
  EXPECT_FALSE(StderrSupportsProgress());
  EXPECT_FALSE(StderrSupportsColor());
  {
    ProgressReporter progress;
    progress.Update("Loading trace: 1 MB");
  }
  EXPECT_TRUE(Output().empty());
}

TEST_F(ProgressReporterTest, ForceColorDoesNotForceProgress) {
  // Any nonempty value, including "0", forces ANSI color.
  SetEnv("FORCE_COLOR", "0");
  SetEnv("TERM", "dumb");
  EXPECT_TRUE(StderrSupportsColor());
  EXPECT_FALSE(StderrSupportsProgress());
  {
    ProgressReporter progress;
    progress.Update("Loading trace: 1 MB");
  }
  EXPECT_TRUE(Output().empty());
}

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX)
TEST_F(ProgressReporterTest, TerminalClipsAndClearsBeforeLogging) {
  SetEnv("TERM", "xterm");
  SetEnv("NO_COLOR", "1");
  auto output = OnTerminal([] {
    EXPECT_TRUE(StderrSupportsProgress());
    EXPECT_FALSE(StderrSupportsColor());
    ProgressReporter progress;
    progress.Update("1234567890123456789overflow");
    PERFETTO_LOG("diagnostic");
  });
  EXPECT_THAT(output, testing::HasSubstr("\r1234567890123456789"));
  EXPECT_THAT(output, testing::Not(testing::HasSubstr("overflow")));
  EXPECT_THAT(output, testing::HasSubstr("\r                   \r"));
  EXPECT_THAT(output, testing::HasSubstr("diagnostic"));
}

TEST_F(ProgressReporterTest, TerminalSuppressionDoesNotSuppressDiagnostics) {
  SetEnv("TERM", "xterm");
  auto output = OnTerminal([] {
    ProgressReporter progress(false);
    progress.Update("hidden progress");
    fprintf(stderr, "visible diagnostic");
  });
  EXPECT_EQ(output, "visible diagnostic");
  SetEnv("TERM", "dumb");
  EXPECT_TRUE(OnTerminal([] {
                EXPECT_FALSE(StderrSupportsProgress());
                ProgressReporter progress;
                progress.Update("hidden progress");
              }).empty());
}
#endif

TEST_F(ProgressReporterTest, ForceColorOverridesNoColor) {
  SetEnv("NO_COLOR", "1");
  EXPECT_FALSE(StderrSupportsColor());
  SetEnv("FORCE_COLOR", "1");
  EXPECT_TRUE(StderrSupportsColor());
  SetEnv("FORCE_COLOR", "");
  EXPECT_FALSE(StderrSupportsColor());
}
}  // namespace
}  // namespace perfetto::base
