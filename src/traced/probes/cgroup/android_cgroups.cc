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
// TODO(rsavitski): move the glob_aware parts into a common directory.
#include "src/profiling/common/proc_utils.h"

namespace perfetto {
namespace {

using AndroidScopePb = protos::pbzero::MemcgConfig::AndroidScope;
using Group = AndroidCgroup::Group;
using Level = AndroidCgroup::Level;

constexpr char kUidPrefix[] = "uid_";
constexpr char kPidPrefix[] = "pid_";

// Parses the id out of a name of the form "<prefix><id>".
std::optional<uint32_t> ParsePrefixedId(std::string_view name,
                                        std::string_view prefix) {
  if (name.substr(0, prefix.size()) != prefix)
    return std::nullopt;
  return base::StringViewToNumber<uint32_t>(name.substr(prefix.size()));
}

// Calls |fn| for each "<prefix><id>" subdirectory of |path|.
template <typename Fn>
void ForEachSubdirId(const std::string& path, std::string_view prefix, Fn fn) {
  base::ScopedDir dir(opendir(path.c_str()));
  if (!dir)
    return;
  while (struct dirent* entry = readdir(*dir)) {
    if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN)
      continue;
    if (std::optional<uint32_t> id = ParsePrefixedId(entry->d_name, prefix);
        id) {
      fn(*id, std::string_view(entry->d_name));
    }
  }
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
    if (group < Group::GROUP_APPS || group > Group::GROUP_SYSTEM) {
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
  if (scope_pb.has_package_name())
    scope.app_ids.emplace();
  for (auto it = scope_pb.package_name(); it; ++it) {
    std::string name = (*it).ToStdString();
    if (const uint32_t* app_id = package_app_ids.Find(name); app_id) {
      scope.app_ids->insert(*app_id);
    } else {
      PERFETTO_DLOG("linux.cgroup: package %s isn't installed", name.c_str());
    }
  }

  // Pid and cmdline.
  for (auto it = scope_pb.pid(); it; ++it)
    scope.pids.insert(*it);
  for (auto it = scope_pb.target_cmdline(); it; ++it)
    scope.cmdline_patterns.push_back((*it).ToStdString());

  // Levels.
  for (auto it = scope_pb.level(); it; ++it) {
    auto level = static_cast<Level>(*it);
    if (level < Level::LEVEL_TOP || level > Level::LEVEL_PID) {
      PERFETTO_ELOG("linux.cgroup: ignoring unknown android_scope level %d",
                    static_cast<int>(level));
      continue;
    }
    scope.levels.insert(level);
  }
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

bool AndroidCgroupScope::MatchesPid(uint32_t pid,
                                    const std::string& cmdline) const {
  if (!pids.empty() && pids.count(pid) == 0)
    return false;
  if (cmdline_patterns.empty())
    return true;
  if (cmdline.empty())
    return false;
  return profiling::glob_aware::MatchCmdlineGlobPatterns(cmdline,
                                                         cmdline_patterns);
}

void VisitSelectedCgroups(const std::string& root,
                          const std::vector<AndroidCgroupScope>& scopes,
                          const ReadCmdlineFn& read_cmdline,
                          const AndroidCgroupVisitor& visit) {
  std::string group_path;
  std::string uid_path;
  std::string pid_path;
  std::string cmdline;

  for (const auto& entry : kGroups) {
    Group group = entry.group;
    // Path should exist unless we're sideloading.
    group_path.assign(root).append("/").append(entry.dir);
    bool list_uids = false;
    bool group_selected = false;
    for (const AndroidCgroupScope& scope : scopes) {
      if (!scope.MatchesGroup(group))
        continue;
      // Only list the per-uid subdirs if filtering or recording at a narrower
      // level.
      list_uids |= scope.HasUidFilter() || scope.HasPidFilter() ||
                   scope.HasLevel(Level::LEVEL_UID) ||
                   scope.HasLevel(Level::LEVEL_PID);
      // Simple case: no narrower filters and asked to record the group.
      if (!scope.HasUidFilter() && !scope.HasPidFilter()) {
        group_selected |= scope.HasLevel(Level::LEVEL_TOP);
      }
    }
    if (!list_uids && !group_selected)
      continue;

    auto walk_uid = [&](uint32_t uid, std::string_view uid_dir) {
      uid_path.assign(group_path).append("/").append(uid_dir);
      bool list_pids = false;
      bool need_cmdline = false;
      bool uid_selected = false;
      for (const AndroidCgroupScope& scope : scopes) {
        if (!(scope.MatchesGroup(group) && scope.MatchesUid(uid)))
          continue;
        // Only list the per-pid subdirs if filtering or recording at pid level.
        list_pids |= scope.HasPidFilter() || scope.HasLevel(Level::LEVEL_PID);
        need_cmdline |= !scope.cmdline_patterns.empty();
        // Passed the filter if not filtering by pid/cmdline.
        if (!scope.HasPidFilter()) {
          uid_selected |= scope.HasLevel(Level::LEVEL_UID);
          group_selected |= scope.HasLevel(Level::LEVEL_TOP);
        }
      }

      auto walk_pid = [&](uint32_t pid, std::string_view pid_dir) {
        // Read /proc/pid/cmdline (once) if filtering by it.
        if (!need_cmdline || !read_cmdline(pid, &cmdline)) {
          cmdline.clear();
        }
        bool pid_selected = false;
        for (const AndroidCgroupScope& scope : scopes) {
          if (!(scope.MatchesGroup(group) && scope.MatchesUid(uid) &&
                scope.MatchesPid(pid, cmdline))) {
            continue;
          }
          // Passed all levels of filtering.
          pid_selected |= scope.HasLevel(Level::LEVEL_PID);
          uid_selected |= scope.HasLevel(Level::LEVEL_UID);
          group_selected |= scope.HasLevel(Level::LEVEL_TOP);
        }
        if (pid_selected) {
          pid_path.assign(uid_path).append("/").append(pid_dir);
          visit(AndroidCgroup{Level::LEVEL_PID, group, uid, pid}, pid_path);
        }
      };  // walk_pid

      if (list_pids) {
        ForEachSubdirId(uid_path, kPidPrefix, walk_pid);
      }
      if (uid_selected) {
        visit(AndroidCgroup{Level::LEVEL_UID, group, uid}, uid_path);
      }
    };  // walk_uid

    if (list_uids) {
      ForEachSubdirId(group_path, kUidPrefix, walk_uid);
    }
    if (group_selected) {
      visit(AndroidCgroup{Level::LEVEL_TOP, group}, group_path);
    }
  }
}

}  // namespace perfetto
