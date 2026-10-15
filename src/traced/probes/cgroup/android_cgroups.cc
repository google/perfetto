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

#include <dirent.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/string_utils.h"
#include "protos/perfetto/config/linux/cgroup_config.pbzero.h"
// TODO(rsavitski): move the glob_aware parts into a more common directory.
#include "src/profiling/common/proc_utils.h"

namespace perfetto {
namespace {

using AndroidScopePb = protos::pbzero::MemcgConfig::AndroidScope;
using Group = AndroidCgroup::Group;
using Level = AndroidCgroup::Level;

constexpr char kUidPrefix[] = "uid_";
constexpr char kPidPrefix[] = "pid_";

// Parses directory names of the form "<prefix><number>".
std::optional<uint32_t> ParseDirName(std::string_view name,
                                     std::string_view prefix) {
  if (name.substr(0, prefix.size()) != prefix)
    return std::nullopt;
  return base::StringViewToNumber<uint32_t>(name.substr(prefix.size()));
}

// Lists the ids of the "<prefix><number>" subdirectories of |path|, sorted.
// Empty if |path| can't be opened, e.g. because it was removed.
std::vector<uint32_t> ListSubdirIds(const std::string& path,
                                    std::string_view prefix) {
  std::vector<uint32_t> ids;
  base::ScopedDir dir(opendir(path.c_str()));
  if (!dir)
    return ids;
  while (struct dirent* entry = readdir(*dir)) {
    if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN)
      continue;
    if (std::optional<uint32_t> id = ParseDirName(entry->d_name, prefix); id)
      ids.push_back(*id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

// The top-level groups of the hierarchy, and their directories.
constexpr struct {
  Group group;
  const char* dir;
} kGroups[] = {
    {Group::GROUP_APPS, "apps"},
    {Group::GROUP_SYSTEM, "system"},
};

// The level recorded if a scope doesn't list any: the level of its narrowest
// filter, or the uid level if it has no filters.
Level DefaultLevel(const AndroidCgroupScope& scope) {
  if (scope.HasPidFilter())
    return Level::LEVEL_PID;
  if (scope.HasUidFilter())
    return Level::LEVEL_UID;
  if (!scope.groups.empty())
    return Level::LEVEL_TOP;
  return Level::LEVEL_UID;
}

bool CmdlineMatches(uint32_t pid,
                    const std::vector<std::string>& patterns,
                    const ReadCmdlineFn& read_cmdline) {
  std::string cmdline;
  if (!read_cmdline(pid, &cmdline) || cmdline.empty())
    return false;
  return profiling::glob_aware::MatchCmdlineGlobPatterns(cmdline, patterns);
}

// Returns true if |pid| is in scope, explicitly or by cmdline.
bool MatchesPid(const AndroidCgroupScope& scope,
                uint32_t pid,
                const ReadCmdlineFn& read_cmdline) {
  if (!scope.pids.empty() && !scope.pids.count(pid))
    return false;
  // Checked last, since it reads the cmdline.
  return scope.cmdline_patterns.empty() ||
         CmdlineMatches(pid, scope.cmdline_patterns, read_cmdline);
}

}  // namespace

// static
std::optional<AndroidCgroupScope> AndroidCgroupScope::Create(
    protozero::ConstBytes scope_bytes,
    const base::FlatHashMap<std::string, uint32_t>& package_app_ids) {
  AndroidScopePb::Decoder scope_pb(scope_bytes);
  AndroidCgroupScope scope;

  // Groups (system or apps).
  for (auto it = scope_pb.group(); it; ++it) {
    auto group = static_cast<Group>(*it);
    if (group != Group::GROUP_APPS && group != Group::GROUP_SYSTEM) {
      PERFETTO_ELOG("linux.cgroup: ignoring unknown android_scope group %d",
                    static_cast<int>(group));
      continue;
    }
    scope.groups.insert(group);
  }
  if (scope_pb.has_group() && scope.groups.empty()) {
    PERFETTO_ELOG("linux.cgroup: android_scope has no known groups");
    return std::nullopt;
  }

  // Uid and package name.
  for (auto it = scope_pb.uid(); it; ++it)
    scope.uids.insert(*it);
  for (auto it = scope_pb.package_name(); it; ++it) {
    std::string name = (*it).ToStdString();
    if (const uint32_t* app_id = package_app_ids.Find(name); app_id) {
      scope.app_ids.insert(*app_id);
    } else {
      PERFETTO_DLOG("linux.cgroup: package %s isn't installed", name.c_str());
    }
    scope.package_names.push_back(std::move(name));
  }

  // Pid and cmdline.
  for (auto it = scope_pb.pid(); it; ++it)
    scope.pids.insert(*it);
  for (auto it = scope_pb.target_cmdline(); it; ++it)
    scope.cmdline_patterns.push_back((*it).ToStdString());

  for (auto it = scope_pb.level(); it; ++it) {
    auto level = static_cast<Level>(*it);
    if (level < Level::LEVEL_TOP || level > Level::LEVEL_PID) {
      PERFETTO_ELOG("linux.cgroup: ignoring unknown android_scope level %d",
                    static_cast<int>(level));
      continue;
    }
    scope.levels.insert(level);
  }

  // Levels.
  if (scope.levels.empty()) {
    if (scope_pb.has_level()) {
      PERFETTO_ELOG("linux.cgroup: android_scope has no known levels");
      return std::nullopt;
    }
    scope.levels.insert(DefaultLevel(scope));
  }
  return scope;
}

// static
AndroidCgroupScope AndroidCgroupScope::Default() {
  AndroidCgroupScope scope;
  scope.levels.insert(Level::LEVEL_UID);
  return scope;
}

void VisitSelectedCgroups(const std::string& root,
                          const std::vector<AndroidCgroupScope>& scopes,
                          const ReadCmdlineFn& read_cmdline,
                          const AndroidCgroupVisitor& visit) {
  for (const auto& [group, dir] : kGroups) {
    // Whether any scope covers the group, and whether one selects the group
    // itself regardless of its uids (no filters below the group level).
    bool group_matched = false;
    bool group_selected = false;
    for (const AndroidCgroupScope& scope : scopes) {
      if (!scope.MatchesGroup(group))
        continue;
      group_matched = true;
      group_selected |= scope.HasLevel(Level::LEVEL_TOP) &&
                        !scope.HasUidFilter() && !scope.HasPidFilter();
    }
    if (!group_matched)
      continue;

    // List group/uid_*.
    std::string group_path = root + "/" + dir;
    std::vector<uint32_t> uids = ListSubdirIds(group_path, kUidPrefix);
    if (uids.empty())
      continue;

    for (uint32_t uid : uids) {
      std::string uid_path =
          group_path + "/" + kUidPrefix + std::to_string(uid);

      bool uid_selected = false;
      std::optional<std::vector<uint32_t>> pids;
      base::FlatSet<uint32_t> selected_pids;
      for (const AndroidCgroupScope& scope : scopes) {
        if (!scope.MatchesGroup(group) || !scope.MatchesUid(uid))
          continue;
        // Only look at per-pid directories if we're recording or filtering by
        // pid/cmdline.
        bool pid_matched = false;
        if (scope.HasLevel(Level::LEVEL_PID) || scope.HasPidFilter()) {
          // List group/uid/pid_* (reusing it if there are multiple scopes).
          if (!pids.has_value())
            pids = ListSubdirIds(uid_path, kPidPrefix);
          for (uint32_t pid : *pids) {
            if (!MatchesPid(scope, pid, read_cmdline))
              continue;
            pid_matched = true;
            if (scope.HasLevel(Level::LEVEL_PID))
              selected_pids.insert(pid);
          }
        }
        // With pid filters, a uid matches if it contains a matching pid.
        if (scope.HasPidFilter() && !pid_matched)
          continue;
        // Matching one of its uids also selects the group, at LEVEL_TOP.
        group_selected |= scope.HasLevel(Level::LEVEL_TOP);
        uid_selected |= scope.HasLevel(Level::LEVEL_UID);
      }

      if (uid_selected)
        visit(AndroidCgroup{Level::LEVEL_UID, group, uid}, uid_path);
      for (uint32_t pid : selected_pids) {
        visit(AndroidCgroup{Level::LEVEL_PID, group, uid, pid},
              uid_path + "/" + kPidPrefix + std::to_string(pid));
      }
    }  // for (uids)

    if (group_selected) {
      visit(AndroidCgroup{Level::LEVEL_TOP, group}, group_path);
    }
  }  // for (groups)
}

}  // namespace perfetto
