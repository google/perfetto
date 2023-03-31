/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/profiling/perf/perf_producer.h"

#include <stdint.h>
#include <optional>

#include "perfetto/base/logging.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace profiling {
namespace {

bool ShouldReject(pid_t pid,
                  std::string cmdline,
                  const TargetFilter& filter,
                  bool skip_cmd,
                  base::FlatSet<std::string>* additional_cmdlines) {
  return PerfProducer::ShouldRejectDueToFilter(
      pid, filter, skip_cmd, additional_cmdlines, [cmdline](std::string* out) {
        *out = cmdline;
        return true;
      });
}

TEST(TargetFilterTest, EmptyFilter) {
  {
    bool skip_cmd = false;
    base::FlatSet<std::string> extra_cmds;
    TargetFilter filter;

    // empty filter allows everything
    EXPECT_FALSE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
    EXPECT_FALSE(ShouldReject(77, "/bin/echo", filter, skip_cmd, &extra_cmds));
  }
  {
    bool skip_cmd = false;
    base::FlatSet<std::string> extra_cmds;
    TargetFilter filter;
    filter.exclude_pids.insert(1);

    // allow everything besides the explicit exclusions
    EXPECT_FALSE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
    EXPECT_FALSE(ShouldReject(77, "/bin/echo", filter, skip_cmd, &extra_cmds));

    EXPECT_TRUE(ShouldReject(1, "/sbin/init", filter, skip_cmd, &extra_cmds));
  }
}

TEST(TargetFilterTest, TargetPids) {
  bool skip_cmd = false;
  base::FlatSet<std::string> extra_cmds;
  TargetFilter filter;
  filter.pids.insert(32);
  filter.pids.insert(42);

  EXPECT_FALSE(ShouldReject(32, "/bin/echo", filter, skip_cmd, &extra_cmds));
  EXPECT_FALSE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));

  EXPECT_TRUE(ShouldReject(77, "/bin/echo", filter, skip_cmd, &extra_cmds));
}

TEST(TargetFilterTest, ExcludePids) {
  bool skip_cmd = false;
  base::FlatSet<std::string> extra_cmds;
  TargetFilter filter;
  filter.exclude_pids.insert(32);
  filter.exclude_pids.insert(42);

  EXPECT_FALSE(ShouldReject(77, "/bin/echo", filter, skip_cmd, &extra_cmds));

  EXPECT_TRUE(ShouldReject(32, "/bin/echo", filter, skip_cmd, &extra_cmds));
  EXPECT_TRUE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
}

TEST(TargetFilterTest, TargetCmdlines) {
  {
    bool skip_cmd = false;
    base::FlatSet<std::string> extra_cmds;
    TargetFilter filter;
    filter.cmdlines.emplace_back("echo");
    filter.cmdlines.emplace_back("/bin/cat");

    EXPECT_FALSE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
    EXPECT_FALSE(ShouldReject(42, "/bin/cat", filter, skip_cmd, &extra_cmds));

    EXPECT_TRUE(ShouldReject(42, "/bin/top", filter, skip_cmd, &extra_cmds));
  }
  {
    bool skip_cmd = true;
    base::FlatSet<std::string> extra_cmds;
    TargetFilter filter;
    filter.cmdlines.emplace_back("echo");
    filter.cmdlines.emplace_back("/bin/cat");

    // As above but with |skip_cmd| making none of the cmdline checks apply.
    // Therefore everything gets rejected because it's still considered to be a
    // filter that only requested specific targets (and none of these match).
    EXPECT_TRUE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
    EXPECT_TRUE(ShouldReject(42, "/bin/cat", filter, skip_cmd, &extra_cmds));
    EXPECT_TRUE(ShouldReject(42, "/bin/top", filter, skip_cmd, &extra_cmds));
  }
}

TEST(TargetFilterTest, ExcludeCmdlines) {
  bool skip_cmd = false;
  base::FlatSet<std::string> extra_cmds;
  TargetFilter filter;
  filter.exclude_cmdlines.emplace_back("echo");
  filter.exclude_cmdlines.emplace_back("/bin/cat");

  EXPECT_FALSE(ShouldReject(42, "/bin/top", filter, skip_cmd, &extra_cmds));

  EXPECT_TRUE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
  EXPECT_TRUE(ShouldReject(42, "/bin/cat", filter, skip_cmd, &extra_cmds));
}

TEST(TargetFilterTest, ExclusionPrioritised) {
  bool skip_cmd = false;
  base::FlatSet<std::string> extra_cmds;
  TargetFilter filter;
  filter.pids.insert(42);
  filter.exclude_pids.insert(42);
  filter.cmdlines.push_back("echo");
  filter.exclude_cmdlines.push_back("echo");

  EXPECT_TRUE(ShouldReject(42, "/bin/cat", filter, skip_cmd, &extra_cmds));
  EXPECT_TRUE(ShouldReject(100, "/bin/echo", filter, skip_cmd, &extra_cmds));
}

TEST(TargetFilterTest, ProcessSharding) {
  {
    bool skip_cmd = false;
    base::FlatSet<std::string> extra_cmds;
    TargetFilter filter;
    filter.process_sharding =
        ProcessSharding{/*shard_count=*/4, /*chosen_shard=*/1};

    EXPECT_FALSE(ShouldReject(1, "/bin/echo", filter, skip_cmd, &extra_cmds));
    EXPECT_FALSE(ShouldReject(41, "/bin/echo", filter, skip_cmd, &extra_cmds));

    EXPECT_TRUE(ShouldReject(0, "/bin/echo", filter, skip_cmd, &extra_cmds));
    EXPECT_TRUE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
  }
  {
    // as above but with an explicit exclude_pid
    bool skip_cmd = false;
    base::FlatSet<std::string> extra_cmds;
    TargetFilter filter;
    filter.exclude_pids.insert(41);
    filter.process_sharding =
        ProcessSharding{/*shard_count=*/4, /*chosen_shard=*/1};

    EXPECT_FALSE(ShouldReject(1, "/bin/echo", filter, skip_cmd, &extra_cmds));

    // explicit exclusion applies even if pid is in the accepted shard
    EXPECT_TRUE(ShouldReject(41, "/bin/echo", filter, skip_cmd, &extra_cmds));
    EXPECT_TRUE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
  }
}

TEST(TargetFilterTest, AdditionalCmdlines) {
  bool skip_cmd = false;
  base::FlatSet<std::string> extra_cmds;
  TargetFilter filter;
  filter.additional_cmdline_count = 2;

  // first two distinct cmdlines remembered and allowed:
  EXPECT_FALSE(ShouldReject(42, "/bin/echo", filter, skip_cmd, &extra_cmds));
  EXPECT_FALSE(ShouldReject(43, "/bin/echo", filter, skip_cmd, &extra_cmds));
  EXPECT_FALSE(ShouldReject(44, "/bin/cat", filter, skip_cmd, &extra_cmds));

  // further cmdlines rejected:
  EXPECT_TRUE(ShouldReject(45, "/bin/top", filter, skip_cmd, &extra_cmds));

  // remembered cmdlines still allowed:
  EXPECT_FALSE(ShouldReject(46, "/bin/echo", filter, skip_cmd, &extra_cmds));

  EXPECT_EQ(extra_cmds.size(), 2u);
  EXPECT_EQ(extra_cmds.count("/bin/echo"), 1u);
  EXPECT_EQ(extra_cmds.count("/bin/cat"), 1u);
  EXPECT_EQ(extra_cmds.count("/bin/top"), 0u);
}

}  // namespace
}  // namespace profiling
}  // namespace perfetto
