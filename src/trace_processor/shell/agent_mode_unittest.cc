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

#include "src/trace_processor/shell/agent_mode.h"

#include <cstdlib>
#include <optional>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::shell {
namespace {

// Sets (or, with a null |value|, unsets) an environment variable for the
// duration of a test. The tests may well be run by a coding agent, so the
// variables it sets must be controlled.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* old = getenv(name); old) {
      old_ = old;
    }
    if (value) {
      base::SetEnv(name, value);
    } else {
      base::UnsetEnv(name);
    }
  }
  ~ScopedEnv() {
    if (old_) {
      base::SetEnv(name_, *old_);
    } else {
      base::UnsetEnv(name_);
    }
  }

 private:
  std::string name_;
  std::optional<std::string> old_;
};

// Simulates running under Claude Code: CLAUDECODE set and the generic markers
// (which are checked first) unset.
struct ClaudeCodeEnv {
  ScopedEnv ai_agent{"AI_AGENT", nullptr};
  ScopedEnv agent{"AGENT", nullptr};
  ScopedEnv claudecode{"CLAUDECODE", "1"};
};

TEST(AgentModeTest, Detection) {
  ClaudeCodeEnv env;
  AgentMode mode = DecideAgentMode(std::nullopt, /*explicit_format=*/false,
                                   /*stdout_is_tty=*/false);
  EXPECT_TRUE(mode.enabled);
  EXPECT_EQ(mode.detected_env_var, "CLAUDECODE");

  // People at a terminal get the normal output, even inside an agent's
  // environment (e.g. a terminal opened by the agent's IDE).
  EXPECT_FALSE(DecideAgentMode(std::nullopt, /*explicit_format=*/false,
                               /*stdout_is_tty=*/true)
                   .enabled);

  // An explicit output format is a deliberate choice: leave it alone.
  EXPECT_FALSE(DecideAgentMode(std::nullopt, /*explicit_format=*/true,
                               /*stdout_is_tty=*/false)
                   .enabled);
}

TEST(AgentModeTest, FlagsOverrideDetection) {
  ClaudeCodeEnv env;
  EXPECT_FALSE(DecideAgentMode(false, /*explicit_format=*/false,
                               /*stdout_is_tty=*/false)
                   .enabled);

  AgentMode on = DecideAgentMode(true, /*explicit_format=*/true,
                                 /*stdout_is_tty=*/true);
  EXPECT_TRUE(on.enabled);
  EXPECT_EQ(on.detected_env_var, std::nullopt);
}

TEST(AgentModeTest, PlainErrorAsJson) {
  EXPECT_EQ(FormatErrorAsJson(base::ErrStatus("could not open \"x\"")),
            R"({"error":"could not open \"x\""})");
}

TEST(AgentModeTest, SqlErrorAsJson) {
  base::Status status = base::ErrStatus(R"( --> query:1:8
  |
1 | select foo from slice
  |        ^
error: no such column: foo)");
  status.SetPayload(kFailedStatementPayload, "2");
  EXPECT_EQ(
      FormatErrorAsJson(status),
      R"({"error":"no such column: foo","statement":2,"location":"query:1:8",)"
      R"("traceback":" --> query:1:8\n  |\n1 | select foo from slice\n  | )"
      R"(       ^\n","hint":"`SELECT * FROM <table> LIMIT 0` prints a )"
      R"(table's columns"})");
}

TEST(AgentModeTest, NestedSqlErrorLocationIsTheUsersQuery) {
  base::Status status = base::ErrStatus(R"( --> query:1:1
  |
1 | include perfetto module foo.bar
  | ^
 --> module foo.bar:1:8
  |
1 | select t from slice
  |        ^
error: no such table: slice)");
  std::string json = FormatErrorAsJson(status);
  EXPECT_THAT(json, testing::HasSubstr(R"("location":"query:1:1")"));
  EXPECT_THAT(json, testing::HasSubstr(R"("error":"no such table: slice")"));
  EXPECT_THAT(json, testing::HasSubstr(R"("hint":)"));
}

}  // namespace
}  // namespace perfetto::trace_processor::shell
