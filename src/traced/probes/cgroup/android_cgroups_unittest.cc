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

#include "src/traced/probes/cgroup/android_cgroups.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "protos/perfetto/config/linux/cgroup_config.gen.h"
#include "src/base/test/tmp_dir_tree.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using AndroidScope = protos::gen::MemcgConfig::AndroidScope;
using Group = AndroidCgroup::Group;
using Level = AndroidCgroup::Level;

std::optional<AndroidCgroupScope> CreateScope(
    const AndroidScope& scope_pb,
    const base::FlatHashMap<std::string, uint32_t>& package_app_ids) {
  std::string bytes = scope_pb.SerializeAsString();
  return AndroidCgroupScope::Create(
      protozero::ConstBytes{reinterpret_cast<const uint8_t*>(bytes.data()),
                            bytes.size()},
      package_app_ids);
}

std::optional<AndroidCgroupScope> CreateScope(const AndroidScope& scope_pb) {
  return CreateScope(scope_pb, base::FlatHashMap<std::string, uint32_t>());
}

AndroidScope MakeScope(const std::vector<AndroidScope::Level>& levels,
                       const std::vector<uint32_t>& uids = {},
                       const std::vector<uint32_t>& pids = {}) {
  AndroidScope scope;
  for (auto level : levels)
    scope.add_level(level);
  for (uint32_t uid : uids)
    scope.add_uid(uid);
  for (uint32_t pid : pids)
    scope.add_pid(pid);
  return scope;
}

AndroidScope MakeGroupScope(const std::vector<AndroidScope::Level>& levels,
                            const std::vector<AndroidScope::Group>& groups) {
  AndroidScope scope = MakeScope(levels);
  for (auto group : groups)
    scope.add_group(group);
  return scope;
}

AndroidScope MakePackageScope(const std::vector<AndroidScope::Level>& levels,
                              const std::vector<std::string>& packages) {
  AndroidScope scope = MakeScope(levels);
  for (const std::string& package : packages)
    scope.add_package_name(package);
  return scope;
}

AndroidScope MakeCmdlineScope(const std::vector<AndroidScope::Level>& levels,
                              const std::vector<std::string>& patterns) {
  AndroidScope scope = MakeScope(levels);
  for (const std::string& pattern : patterns)
    scope.add_target_cmdline(pattern);
  return scope;
}

class AndroidCgroupScopeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Mimics the hierarchy on a device, including the cgroup files.
    tree_.AddDir("apps");
    tree_.AddFile("apps/memory.current", "0\n");
    tree_.AddDir("apps/uid_10001");
    tree_.AddFile("apps/uid_10001/cgroup.procs", "");
    tree_.AddDir("apps/uid_10001/pid_100");
    tree_.AddDir("apps/uid_10001/pid_101");
    // A uid without any processes.
    tree_.AddDir("apps/uid_10002");
    tree_.AddDir("system");
    tree_.AddDir("system/uid_0");
    tree_.AddDir("system/uid_0/pid_1");
    tree_.AddDir("system/uid_1000");
    tree_.AddDir("system/uid_1000/pid_579");
    tree_.AddDir("system/uid_1000/pid_600");

    package_app_ids_.Insert("com.example.foo", 10001);
    package_app_ids_.Insert("com.example.bar", 10002);
    package_app_ids_.Insert("android", 1000);

    // As read from /proc/<pid>/cmdline.
    cmdlines_[1] = std::string("/system/bin/init\0second_stage\0", 30);
    cmdlines_[100] = std::string("com.example.foo\0", 16);
    cmdlines_[101] = std::string("com.example.foo:remote\0", 23);
    cmdlines_[579] = std::string("/system/bin/surfaceflinger\0", 27);
    // pid 600 has an unreadable cmdline.
    read_cmdline_ = [this](uint32_t pid, std::string* cmdline) {
      auto it = cmdlines_.find(pid);
      if (it == cmdlines_.end())
        return false;
      *cmdline = it->second;
      return true;
    };
  }

  // Returns the paths of the selected cgroups relative to |root|, in the
  // order they are visited.
  std::vector<std::string> Visit(const std::vector<AndroidScope>& scope_pbs,
                                 const std::string& root) {
    std::vector<AndroidCgroupScope> scopes;
    for (const AndroidScope& scope_pb : scope_pbs) {
      std::optional<AndroidCgroupScope> scope =
          CreateScope(scope_pb, package_app_ids_);
      EXPECT_TRUE(scope.has_value());
      scopes.push_back(*scope);
    }
    std::vector<std::string> paths;
    VisitSelectedCgroups(root, scopes, read_cmdline_,
                         [&](const AndroidCgroup&, const std::string& path) {
                           paths.push_back(path.substr(root.size() + 1));
                         });
    return paths;
  }

  // Same as Visit(), but sorted.
  std::vector<std::string> Select(const std::vector<AndroidScope>& scope_pbs) {
    std::vector<std::string> paths = Visit(scope_pbs, tree_.path());
    std::sort(paths.begin(), paths.end());
    return paths;
  }

  base::TmpDirTree tree_;
  base::FlatHashMap<std::string, uint32_t> package_app_ids_;
  std::map<uint32_t, std::string> cmdlines_;
  ReadCmdlineFn read_cmdline_;
};

TEST(AndroidCgroupScopeCreateTest, DefaultsToUidLevel) {
  std::optional<AndroidCgroupScope> scope = CreateScope(AndroidScope());
  ASSERT_TRUE(scope.has_value());
  EXPECT_TRUE(scope->HasLevel(Level::LEVEL_UID));
  EXPECT_FALSE(scope->HasLevel(Level::LEVEL_TOP));
  EXPECT_FALSE(scope->HasLevel(Level::LEVEL_PID));
  EXPECT_TRUE(scope->groups.empty());
  EXPECT_FALSE(scope->HasUidFilter());
  EXPECT_FALSE(scope->HasPidFilter());
}

TEST(AndroidCgroupScopeCreateTest, DefaultLevelFollowsNarrowestFilter) {
  auto default_level = [](const AndroidScope& scope_pb) {
    std::optional<AndroidCgroupScope> scope = CreateScope(scope_pb);
    EXPECT_TRUE(scope.has_value());
    if (scope->levels.size() != 1)
      return std::optional<Level>();
    return std::optional<Level>(*scope->levels.begin());
  };
  EXPECT_EQ(default_level(MakeGroupScope({}, {AndroidScope::GROUP_SYSTEM})),
            Level::LEVEL_TOP);
  EXPECT_EQ(default_level(MakeScope({}, {1000})), Level::LEVEL_UID);
  EXPECT_EQ(default_level(MakePackageScope({}, {"com.example.foo"})),
            Level::LEVEL_UID);
  EXPECT_EQ(default_level(MakeScope({}, {}, {579})), Level::LEVEL_PID);
  EXPECT_EQ(default_level(MakeCmdlineScope({}, {"surfaceflinger"})),
            Level::LEVEL_PID);

  AndroidScope scope = MakeGroupScope({}, {AndroidScope::GROUP_APPS});
  scope.add_package_name("com.example.foo");
  EXPECT_EQ(default_level(scope), Level::LEVEL_UID);
  scope.add_target_cmdline("com.example.foo");
  EXPECT_EQ(default_level(scope), Level::LEVEL_PID);
}

TEST(AndroidCgroupScopeCreateTest, RejectsOnlyUnknownGroups) {
  EXPECT_FALSE(
      CreateScope(MakeGroupScope({}, {AndroidScope::GROUP_UNSPECIFIED}))
          .has_value());
  EXPECT_FALSE(
      CreateScope(MakeGroupScope({}, {static_cast<AndroidScope::Group>(42)}))
          .has_value());
  // Unknown groups are ignored if there are also known ones.
  std::optional<AndroidCgroupScope> scope = CreateScope(MakeGroupScope(
      {}, {static_cast<AndroidScope::Group>(42), AndroidScope::GROUP_APPS}));
  ASSERT_TRUE(scope.has_value());
  EXPECT_TRUE(scope->MatchesGroup(Group::GROUP_APPS));
  EXPECT_FALSE(scope->MatchesGroup(Group::GROUP_SYSTEM));
}

TEST(AndroidCgroupScopeCreateTest, RejectsOnlyUnknownLevels) {
  EXPECT_FALSE(CreateScope(MakeScope({static_cast<AndroidScope::Level>(42)}))
                   .has_value());
  // Unknown levels are ignored if there are also known ones.
  std::optional<AndroidCgroupScope> scope = CreateScope(MakeScope(
      {static_cast<AndroidScope::Level>(42), AndroidScope::LEVEL_PID}));
  ASSERT_TRUE(scope.has_value());
  EXPECT_TRUE(scope->HasLevel(Level::LEVEL_PID));
}

TEST_F(AndroidCgroupScopeTest, VisitOrderAndIdentity) {
  std::vector<AndroidCgroupScope> scopes = {
      *CreateScope(MakeScope({AndroidScope::LEVEL_TOP, AndroidScope::LEVEL_UID,
                              AndroidScope::LEVEL_PID},
                             {1000}))};
  std::vector<std::string> visited;
  VisitSelectedCgroups(
      tree_.path(), scopes, read_cmdline_,
      [&](const AndroidCgroup& cgroup, const std::string& path) {
        visited.push_back(std::to_string(static_cast<int>(cgroup.level)) + " " +
                          std::to_string(cgroup.uid) + " " +
                          std::to_string(cgroup.pid) + " " +
                          path.substr(tree_.path().size() + 1));
      });
  // The uid and pid cgroups are visited before their group.
  EXPECT_THAT(visited, ElementsAre("2 1000 0 system/uid_1000",
                                   "3 1000 579 system/uid_1000/pid_579",
                                   "3 1000 600 system/uid_1000/pid_600",
                                   "1 0 0 system"));
}

TEST_F(AndroidCgroupScopeTest, IgnoresOtherDirectories) {
  tree_.AddDir("apps/uid_");
  tree_.AddDir("apps/uid_10003x");
  tree_.AddDir("apps/pid_10004");
  EXPECT_THAT(Select({MakeScope({})}),
              ElementsAre("apps/uid_10001", "apps/uid_10002", "system/uid_0",
                          "system/uid_1000"));
  // Missing hierarchy.
  EXPECT_THAT(Visit({MakeScope({})}, tree_.AbsolutePath("apps/uid_10002")),
              IsEmpty());
}

TEST_F(AndroidCgroupScopeTest, AllAtEachLevel) {
  EXPECT_THAT(Select({MakeScope({AndroidScope::LEVEL_TOP})}),
              ElementsAre("apps", "system"));
  EXPECT_THAT(Select({MakeScope({})}),
              ElementsAre("apps/uid_10001", "apps/uid_10002", "system/uid_0",
                          "system/uid_1000"));
  EXPECT_THAT(Select({MakeScope({AndroidScope::LEVEL_PID})}),
              ElementsAre("apps/uid_10001/pid_100", "apps/uid_10001/pid_101",
                          "system/uid_0/pid_1", "system/uid_1000/pid_579",
                          "system/uid_1000/pid_600"));
}

TEST_F(AndroidCgroupScopeTest, GroupFilter) {
  EXPECT_THAT(Select({MakeGroupScope({AndroidScope::LEVEL_TOP},
                                     {AndroidScope::GROUP_SYSTEM})}),
              ElementsAre("system"));
  EXPECT_THAT(Select({MakeGroupScope({AndroidScope::LEVEL_UID},
                                     {AndroidScope::GROUP_SYSTEM})}),
              ElementsAre("system/uid_0", "system/uid_1000"));
  // Default level is top.
  EXPECT_THAT(Select({MakeGroupScope({}, {AndroidScope::GROUP_SYSTEM})}),
              ElementsAre("system"));
  EXPECT_THAT(Select({MakeGroupScope({AndroidScope::LEVEL_PID},
                                     {AndroidScope::GROUP_APPS})}),
              ElementsAre("apps/uid_10001/pid_100", "apps/uid_10001/pid_101"));
  EXPECT_THAT(Select({MakeGroupScope(
                  {AndroidScope::LEVEL_TOP},
                  {AndroidScope::GROUP_APPS, AndroidScope::GROUP_SYSTEM})}),
              ElementsAre("apps", "system"));
}

TEST_F(AndroidCgroupScopeTest, GroupFilterIsPerScope) {
  // Each scope's group filter only applies to that scope.
  EXPECT_THAT(
      Select({MakeGroupScope({AndroidScope::LEVEL_TOP, AndroidScope::LEVEL_UID},
                             {AndroidScope::GROUP_SYSTEM}),
              MakeGroupScope({AndroidScope::LEVEL_PID},
                             {AndroidScope::GROUP_APPS})}),
      ElementsAre("apps/uid_10001/pid_100", "apps/uid_10001/pid_101", "system",
                  "system/uid_0", "system/uid_1000"));
}

TEST_F(AndroidCgroupScopeTest, UidFilter) {
  EXPECT_THAT(Select({MakeScope({AndroidScope::LEVEL_UID}, {1000})}),
              ElementsAre("system/uid_1000"));
  EXPECT_THAT(
      Select({MakeScope({AndroidScope::LEVEL_PID}, {1000})}),
      ElementsAre("system/uid_1000/pid_579", "system/uid_1000/pid_600"));
  EXPECT_THAT(Select({MakeScope({AndroidScope::LEVEL_TOP}, {1000})}),
              ElementsAre("system"));
  // A uid without processes can still be selected at the uid level.
  EXPECT_THAT(
      Select({MakeScope({AndroidScope::LEVEL_TOP, AndroidScope::LEVEL_UID,
                         AndroidScope::LEVEL_PID},
                        {10002})}),
      ElementsAre("apps", "apps/uid_10002"));
  // No match.
  EXPECT_THAT(
      Select({MakeScope({AndroidScope::LEVEL_TOP, AndroidScope::LEVEL_UID},
                        {12345})}),
      IsEmpty());
}

TEST_F(AndroidCgroupScopeTest, PidFilter) {
  EXPECT_THAT(Select({MakeScope({AndroidScope::LEVEL_PID}, {}, {579, 101})}),
              ElementsAre("apps/uid_10001/pid_101", "system/uid_1000/pid_579"));
  // The uid/top cgroups containing the pid.
  EXPECT_THAT(
      Select({MakeScope({AndroidScope::LEVEL_TOP, AndroidScope::LEVEL_UID}, {},
                        {579})}),
      ElementsAre("system", "system/uid_1000"));
  EXPECT_THAT(Select({MakeScope({AndroidScope::LEVEL_UID}, {}, {9999})}),
              IsEmpty());
}

TEST_F(AndroidCgroupScopeTest, UnionAcrossScopes) {
  // Mixed depths for different uids, with an overlap that is deduplicated.
  EXPECT_THAT(Select({MakeScope({AndroidScope::LEVEL_UID}, {1000, 10001}),
                      MakeScope({AndroidScope::LEVEL_PID}, {10001}),
                      MakeScope({AndroidScope::LEVEL_UID}, {}, {100})}),
              ElementsAre("apps/uid_10001", "apps/uid_10001/pid_100",
                          "apps/uid_10001/pid_101", "system/uid_1000"));
}

TEST_F(AndroidCgroupScopeTest, PackageFilter) {
  EXPECT_THAT(Select({MakePackageScope(
                  {AndroidScope::LEVEL_UID, AndroidScope::LEVEL_PID},
                  {"com.example.foo"})}),
              ElementsAre("apps/uid_10001", "apps/uid_10001/pid_100",
                          "apps/uid_10001/pid_101"));
  EXPECT_THAT(Select({MakePackageScope({AndroidScope::LEVEL_TOP},
                                       {"android", "com.example.bar"})}),
              ElementsAre("apps", "system"));
  // Unknown packages match nothing.
  EXPECT_THAT(Select({MakePackageScope({}, {"com.example.unknown"})}),
              IsEmpty());

  // Without packages.list, nothing matches.
  package_app_ids_.Clear();
  EXPECT_THAT(Select({MakePackageScope({}, {"com.example.foo"})}), IsEmpty());
}

TEST_F(AndroidCgroupScopeTest, PackageFilterMatchesAllUsers) {
  // The same app, running in user 10.
  tree_.AddDir("apps/uid_1010001");
  tree_.AddDir("apps/uid_1010001/pid_200");
  EXPECT_THAT(Select({MakePackageScope({}, {"com.example.foo"})}),
              ElementsAre("apps/uid_10001", "apps/uid_1010001"));
}

TEST_F(AndroidCgroupScopeTest, CmdlineFilter) {
  // Matched against the binary name.
  EXPECT_THAT(Select({MakeCmdlineScope({AndroidScope::LEVEL_PID},
                                       {"com.example.foo"})}),
              ElementsAre("apps/uid_10001/pid_100"));
  EXPECT_THAT(Select({MakeCmdlineScope({AndroidScope::LEVEL_PID},
                                       {"com.example.foo*"})}),
              ElementsAre("apps/uid_10001/pid_100", "apps/uid_10001/pid_101"));
  EXPECT_THAT(Select({MakeCmdlineScope({AndroidScope::LEVEL_PID},
                                       {"surfaceflinger", "init"})}),
              ElementsAre("system/uid_0/pid_1", "system/uid_1000/pid_579"));
  // Absolute patterns are matched against argv0.
  EXPECT_THAT(
      Select({MakeCmdlineScope({AndroidScope::LEVEL_PID}, {"/system/bin/*"})}),
      ElementsAre("system/uid_0/pid_1", "system/uid_1000/pid_579"));
  // The uid and top cgroups containing the process.
  EXPECT_THAT(Select({MakeCmdlineScope(
                  {AndroidScope::LEVEL_TOP, AndroidScope::LEVEL_UID},
                  {"surfaceflinger"})}),
              ElementsAre("system", "system/uid_1000"));
  // Default level is pid.
  EXPECT_THAT(Select({MakeCmdlineScope({}, {"com.example.foo:remote"})}),
              ElementsAre("apps/uid_10001/pid_101"));
  // Processes with an unreadable or empty (kernel threads and zombies) cmdline
  // never match.
  cmdlines_[600] = "";
  EXPECT_THAT(Select({MakeCmdlineScope({AndroidScope::LEVEL_PID}, {"*"})}),
              ElementsAre("apps/uid_10001/pid_100", "apps/uid_10001/pid_101",
                          "system/uid_0/pid_1", "system/uid_1000/pid_579"));
}

TEST_F(AndroidCgroupScopeTest, FiltersAreAnded) {
  // A process of a given app.
  AndroidScope scope = MakePackageScope({}, {"com.example.foo"});
  scope.add_target_cmdline("*:remote");
  EXPECT_THAT(Select({scope}), ElementsAre("apps/uid_10001/pid_101"));
  // The cmdline matches processes, but not in the package's uid.
  scope = MakePackageScope({}, {"com.example.bar"});
  scope.add_target_cmdline("com.example.foo*");
  EXPECT_THAT(Select({scope}), IsEmpty());
  // A filter that matches nothing (here, a package that isn't installed)
  // selects nothing, rather than being ignored.
  scope = MakePackageScope({}, {"com.example.unknown"});
  scope.add_target_cmdline("*");
  EXPECT_THAT(Select({scope}), IsEmpty());

  // The pid has to be within the uid.
  EXPECT_THAT(Select({MakeScope({}, {10001}, {100, 579})}),
              ElementsAre("apps/uid_10001/pid_100"));
  EXPECT_THAT(Select({MakeScope({}, {1000}, {100})}), IsEmpty());

  // The uid has to be within the group, also for the top level.
  scope = MakeGroupScope({AndroidScope::LEVEL_TOP, AndroidScope::LEVEL_UID},
                         {AndroidScope::GROUP_APPS});
  scope.add_uid(10001);
  scope.add_uid(1000);
  EXPECT_THAT(Select({scope}), ElementsAre("apps", "apps/uid_10001"));
  scope = MakeGroupScope({AndroidScope::LEVEL_TOP}, {AndroidScope::GROUP_APPS});
  scope.add_target_cmdline("surfaceflinger");
  EXPECT_THAT(Select({scope}), IsEmpty());
}

}  // namespace
}  // namespace perfetto
