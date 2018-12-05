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

#include "src/profiling/memory/process_matcher.h"

#include "gtest/gtest.h"

namespace perfetto {
namespace profiling {
namespace {

TEST(MatcherTest, MatchPIDProcessSetSpecFirst) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_TRUE(match);
  EXPECT_FALSE(shutdown);
}

TEST(MatcherTest, MatchPIDProcessSetSpecSecond) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  auto handle = m.ProcessConnected({1, "init"});
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  EXPECT_TRUE(match);
  EXPECT_FALSE(shutdown);
}

TEST(MatcherTest, MatchCmdlineProcessSetSpecFirst) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");

  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_TRUE(match);
  EXPECT_FALSE(shutdown);
}

TEST(MatcherTest, MatchCmdlineProcessSetSpecSecond) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");

  auto handle = m.ProcessConnected({1, "init"});
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  EXPECT_TRUE(match);
  EXPECT_FALSE(shutdown);
}

TEST(MatcherTest, ExpiredProcessSetSpecHandle) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  { auto ps_handle = m.AwaitProcessSetSpec(std::move(ps)); }
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_FALSE(match);
}

TEST(MatcherTest, ExpiredProcessHandle) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  { auto handle = m.ProcessConnected({1, "init"}); }
  EXPECT_FALSE(shutdown);
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  EXPECT_FALSE(match);
}

TEST(MatcherTest, MatchCmdlineProcessSetSpecFirstMultiple) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");
  ProcessSetSpec ps2;
  ps2.process_cmdline.emplace("init");

  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  auto ps2_handle = m.AwaitProcessSetSpec(std::move(ps2));
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_TRUE(match);
  EXPECT_FALSE(shutdown);
  { auto destroy = std::move(ps2_handle); }
  EXPECT_FALSE(shutdown);
  { auto destroy = std::move(ps_handle); }
  EXPECT_TRUE(shutdown);
}

TEST(MatcherTest, GetPIDs) {
  bool match = false;
  auto match_fn = [&match](const Process&,
                           const std::vector<const ProcessSetSpec*>&) {
    match = true;
  };
  bool shutdown = false;
  auto shutdown_fn = [&shutdown](pid_t) { shutdown = true; };

  ProcessMatcher m(std::move(shutdown_fn), std::move(match_fn));
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");

  auto init_handle = m.ProcessConnected({1, "init"});
  auto second_init_handle = m.ProcessConnected({2, "init"});
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  std::set<pid_t> expected_pids{1, 2};
  EXPECT_EQ(ps_handle.GetPIDs(), expected_pids);
  EXPECT_TRUE(match);
  EXPECT_FALSE(shutdown);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
