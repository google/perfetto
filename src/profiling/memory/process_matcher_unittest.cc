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

class DummyDelegate : public ProcessMatcher::Delegate {
 public:
  void Match(const Process&,
             const std::vector<const ProcessSetSpec*>&) override {
    match = true;
  }

  void Disconnect(pid_t) override { shutdown = true; }

  bool match = false;
  bool shutdown = false;
};

TEST(MatcherTest, MatchPIDProcessSetSpecFirst) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_TRUE(delegate.match);
  EXPECT_FALSE(delegate.shutdown);
}

TEST(MatcherTest, MatchPIDProcessSetSpecSecond) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  auto handle = m.ProcessConnected({1, "init"});
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  EXPECT_TRUE(delegate.match);
  EXPECT_FALSE(delegate.shutdown);
}

TEST(MatcherTest, MatchCmdlineProcessSetSpecFirst) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");

  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_TRUE(delegate.match);
  EXPECT_FALSE(delegate.shutdown);
}

TEST(MatcherTest, MatchCmdlineProcessSetSpecSecond) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");

  auto handle = m.ProcessConnected({1, "init"});
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  EXPECT_TRUE(delegate.match);
  EXPECT_FALSE(delegate.shutdown);
}

TEST(MatcherTest, ExpiredProcessSetSpecHandle) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  { auto ps_handle = m.AwaitProcessSetSpec(std::move(ps)); }
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_FALSE(delegate.match);
}

TEST(MatcherTest, ExpiredProcessHandle) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.pids.emplace(1);

  { auto handle = m.ProcessConnected({1, "init"}); }
  EXPECT_FALSE(delegate.shutdown);
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  EXPECT_FALSE(delegate.match);
}

TEST(MatcherTest, MatchCmdlineProcessSetSpecFirstMultiple) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");
  ProcessSetSpec ps2;
  ps2.process_cmdline.emplace("init");

  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  auto ps2_handle = m.AwaitProcessSetSpec(std::move(ps2));
  auto handle = m.ProcessConnected({1, "init"});
  EXPECT_TRUE(delegate.match);
  EXPECT_FALSE(delegate.shutdown);
  { auto destroy = std::move(ps2_handle); }
  EXPECT_FALSE(delegate.shutdown);
  { auto destroy = std::move(ps_handle); }
  EXPECT_TRUE(delegate.shutdown);
}

TEST(MatcherTest, GetPIDs) {
  DummyDelegate delegate;

  ProcessMatcher m(&delegate);
  ProcessSetSpec ps;
  ps.process_cmdline.emplace("init");

  auto init_handle = m.ProcessConnected({1, "init"});
  auto second_init_handle = m.ProcessConnected({2, "init"});
  auto ps_handle = m.AwaitProcessSetSpec(std::move(ps));
  std::set<pid_t> expected_pids{1, 2};
  EXPECT_EQ(ps_handle.GetPIDs(), expected_pids);
  EXPECT_TRUE(delegate.match);
  EXPECT_FALSE(delegate.shutdown);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
