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

#include "src/traced/probes/cgroup/memcg_reader.h"

#include <optional>
#include <string>
#include <vector>

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/config/linux/cgroup_config.gen.h"
#include "protos/perfetto/trace/linux/memcg_stats.gen.h"
#include "protos/perfetto/trace/linux/memcg_stats.pbzero.h"
#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using MC = protos::gen::MemcgConfig;
using MemcgStats = protos::gen::MemcgStats;

// |packages_list_path| is only read if the config has package filters.
std::optional<MemcgReader::Config> CreateConfig(
    const MC& config_pb,
    const std::string& packages_list_path = "") {
  std::string bytes = config_pb.SerializeAsString();
  return MemcgReader::Config::Create(
      protozero::ConstBytes{reinterpret_cast<const uint8_t*>(bytes.data()),
                            bytes.size()},
      packages_list_path);
}

class MemcgReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tree_.AddDir("apps");
    tree_.AddDir("apps/uid_10001");
    tree_.AddFile("apps/uid_10001/memory.current", "123\n");
    tree_.AddFile("apps/uid_10001/memory.high", "max\n");
    tree_.AddFile("apps/uid_10001/memory.stat", "anon 10\nfile 20\nshmem 5\n");
    tree_.AddFile("apps/uid_10001/memory.events.local",
                  "low 0\nhigh 3\nmax 1\noom 0\noom_kill 0\n");
    tree_.AddDir("system");
    tree_.AddDir("system/uid_1000");
    tree_.AddFile("system/uid_1000/memory.current", "456\n");
  }

  MemcgReader CreateReader(MemcgReader::Config config) {
    return MemcgReader(std::move(config), tree_.path());
  }

  // Reads once, and returns the decoded packet.
  MemcgStats Read(MemcgReader* reader) {
    protozero::HeapBuffered<protos::pbzero::MemcgStats> msg;
    reader->Read(msg.get());
    MemcgStats stats;
    EXPECT_TRUE(stats.ParseFromString(msg.SerializeAsString()));
    return stats;
  }

  base::TmpDirTree tree_;
};

TEST_F(MemcgReaderTest, ConfigValidation) {
  MC config_pb;
  config_pb.add_field(MC::MEMCG_CURRENT);
  config_pb.set_poll_period_ms(1);
  std::optional<MemcgReader::Config> config = CreateConfig(config_pb);
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->poll_period_ms, 100u);  // clamped
  // Default scope.
  ASSERT_EQ(config->scopes.size(), 1u);
  EXPECT_TRUE(config->scopes[0].HasLevel(AndroidCgroup::Level::LEVEL_UID));

  // Invalid scope invalidates the whole config.
  config_pb.add_android_scope()->add_level(
      static_cast<MC::AndroidScope::Level>(42));
  EXPECT_FALSE(CreateConfig(config_pb).has_value());
}

TEST_F(MemcgReaderTest, DefaultFields) {
  std::optional<MemcgReader::Config> config = CreateConfig(MC());
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(config->fields.count(), 3u);
  EXPECT_TRUE(config->fields.test(static_cast<size_t>(MC::MEMCG_CURRENT)));
  EXPECT_TRUE(config->fields.test(static_cast<size_t>(MC::MEMCG_STAT_ANON)));
  EXPECT_TRUE(config->fields.test(static_cast<size_t>(MC::MEMCG_STAT_FILE)));

  // Only fields unknown to this build: not replaced with the default ones.
  MC config_pb;
  config_pb.add_field(static_cast<MC::MemcgField>(999));
  EXPECT_FALSE(CreateConfig(config_pb).has_value());
}

TEST_F(MemcgReaderTest, Read) {
  MC config_pb;
  config_pb.add_field(MC::MEMCG_CURRENT);
  config_pb.add_field(MC::MEMCG_HIGH);
  config_pb.add_field(MC::MEMCG_ZSWAP_CURRENT);  // file doesn't exist
  config_pb.add_field(MC::MEMCG_STAT_ANON);
  config_pb.add_field(MC::MEMCG_STAT_SHMEM);
  config_pb.add_field(MC::MEMCG_EVENTS_LOCAL_ALL);
  std::optional<MemcgReader::Config> config = CreateConfig(config_pb);
  ASSERT_TRUE(config.has_value());

  MemcgReader reader = CreateReader(*config);
  MemcgStats stats = Read(&reader);
  ASSERT_EQ(stats.cgroup().size(), 2u);

  const MemcgStats::Cgroup& app = stats.cgroup()[0];
  EXPECT_EQ(app.android_group(), MemcgStats::GROUP_APPS);
  EXPECT_EQ(app.uid(), 10001u);
  EXPECT_FALSE(app.has_pid());
  EXPECT_EQ(app.current(), 123u);
  EXPECT_EQ(app.high(), kMemcgValueMax);
  EXPECT_FALSE(app.has_zswap_current());
  EXPECT_EQ(app.stat().anon(), 10u);
  EXPECT_EQ(app.stat().shmem(), 5u);
  EXPECT_FALSE(app.stat().has_file());
  EXPECT_EQ(app.events_local().high(), 3u);
  EXPECT_EQ(app.events_local().max(), 1u);
  EXPECT_TRUE(app.events_local().has_oom_kill());
  EXPECT_FALSE(app.events_local().has_oom_group_kill());
  EXPECT_FALSE(app.has_events());

  const MemcgStats::Cgroup& system = stats.cgroup()[1];
  EXPECT_EQ(system.android_group(), MemcgStats::GROUP_SYSTEM);
  EXPECT_EQ(system.uid(), 1000u);
  EXPECT_EQ(system.current(), 456u);
  EXPECT_FALSE(system.has_high());
  EXPECT_FALSE(system.has_stat());
}

TEST_F(MemcgReaderTest, TopAndPidLevels) {
  tree_.AddFile("apps/memory.current", "1000\n");
  tree_.AddDir("apps/uid_10001/pid_42");
  tree_.AddFile("apps/uid_10001/pid_42/memory.current", "7\n");

  MC config_pb;
  config_pb.add_field(MC::MEMCG_CURRENT);
  auto* scope = config_pb.add_android_scope();
  scope->add_group(MC::AndroidScope::GROUP_APPS);
  scope->add_level(MC::AndroidScope::LEVEL_TOP);
  scope->add_level(MC::AndroidScope::LEVEL_PID);
  MemcgReader reader = CreateReader(*CreateConfig(config_pb));
  MemcgStats stats = Read(&reader);
  ASSERT_EQ(stats.cgroup().size(), 2u);

  // The group is written after the cgroups below it.
  EXPECT_EQ(stats.cgroup()[0].uid(), 10001u);
  EXPECT_EQ(stats.cgroup()[0].pid(), 42u);
  EXPECT_EQ(stats.cgroup()[0].current(), 7u);

  EXPECT_EQ(stats.cgroup()[1].android_group(), MemcgStats::GROUP_APPS);
  EXPECT_FALSE(stats.cgroup()[1].has_uid());
  EXPECT_FALSE(stats.cgroup()[1].has_pid());
  EXPECT_EQ(stats.cgroup()[1].current(), 1000u);
}

TEST_F(MemcgReaderTest, ReadsLargeFiles) {
  // The wanted key comes after 64KB of keys unknown to this build.
  std::string stat;
  for (size_t i = 0; stat.size() < 64 * 1024; i++)
    stat += "future_key_" + std::to_string(i) + " 1\n";
  stat += "anon 42\n";
  tree_.AddFile("system/uid_1000/memory.stat", stat);

  MC config_pb;
  config_pb.add_field(MC::MEMCG_STAT_ANON);
  config_pb.add_android_scope()->add_uid(1000);
  MemcgReader reader = CreateReader(*CreateConfig(config_pb));
  MemcgStats stats = Read(&reader);
  ASSERT_EQ(stats.cgroup().size(), 1u);
  EXPECT_EQ(stats.cgroup()[0].stat().anon(), 42u);
}

TEST_F(MemcgReaderTest, SkipsCgroupsWithoutValues) {
  MC config_pb;
  config_pb.add_field(MC::MEMCG_STAT_ANON);
  MemcgReader reader = CreateReader(*CreateConfig(config_pb));
  // system/uid_1000 doesn't have a memory.stat.
  MemcgStats stats = Read(&reader);
  ASSERT_EQ(stats.cgroup().size(), 1u);
  EXPECT_EQ(stats.cgroup()[0].uid(), 10001u);
}

TEST_F(MemcgReaderTest, NewCgroupsArePickedUp) {
  MC config_pb;
  config_pb.add_field(MC::MEMCG_CURRENT);
  auto* scope = config_pb.add_android_scope();
  scope->add_level(MC::AndroidScope::LEVEL_PID);
  MemcgReader reader = CreateReader(*CreateConfig(config_pb));
  EXPECT_TRUE(Read(&reader).cgroup().empty());

  tree_.AddDir("apps/uid_10001/pid_42");
  tree_.AddFile("apps/uid_10001/pid_42/memory.current", "7\n");
  MemcgStats stats = Read(&reader);
  ASSERT_EQ(stats.cgroup().size(), 1u);
  EXPECT_EQ(stats.cgroup()[0].pid(), 42u);
  EXPECT_EQ(stats.cgroup()[0].current(), 7u);
}

TEST_F(MemcgReaderTest, PackageScope) {
  tree_.AddFile("packages.list",
                "com.example.foo 10001 0 /data/user/0/com.example.foo "
                "default:targetSdkVersion=34 none 0 1 1 @system\n"
                "com.example.baz 10003 0 /data/user/0/com.example.baz "
                "default:targetSdkVersion=34 none 0 1 1 @system\n");
  MC config_pb;
  config_pb.add_field(MC::MEMCG_CURRENT);
  config_pb.add_android_scope()->add_package_name("com.example.foo");
  config_pb.add_android_scope()->add_package_name("com.example.baz");
  MemcgReader reader = CreateReader(
      *CreateConfig(config_pb, tree_.AbsolutePath("packages.list")));
  MemcgStats stats = Read(&reader);
  ASSERT_EQ(stats.cgroup().size(), 1u);
  EXPECT_EQ(stats.cgroup()[0].uid(), 10001u);
  EXPECT_EQ(stats.cgroup()[0].current(), 123u);

  // baz is installed, but its uid cgroup only appears once it first runs.
  tree_.AddDir("apps/uid_10003");
  tree_.AddFile("apps/uid_10003/memory.current", "7\n");
  stats = Read(&reader);
  ASSERT_EQ(stats.cgroup().size(), 2u);
  EXPECT_EQ(stats.cgroup()[1].uid(), 10003u);
  EXPECT_EQ(stats.cgroup()[1].current(), 7u);
}

TEST_F(MemcgReaderTest, PackagesAreResolvedOnce) {
  MC config_pb;
  config_pb.add_field(MC::MEMCG_CURRENT);
  config_pb.add_android_scope()->add_package_name("com.example.foo");
  // No packages.list yet.
  MemcgReader reader = CreateReader(
      *CreateConfig(config_pb, tree_.AbsolutePath("packages.list")));
  EXPECT_TRUE(Read(&reader).cgroup().empty());

  // Installed after the config was created: not matched.
  tree_.AddFile("packages.list",
                "com.example.foo 10001 0 /data/user/0/com.example.foo "
                "default:targetSdkVersion=34 none 0 1 1 @system\n");
  EXPECT_TRUE(Read(&reader).cgroup().empty());
}

}  // namespace
}  // namespace perfetto
