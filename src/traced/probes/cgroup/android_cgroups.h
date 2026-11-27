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

#ifndef SRC_TRACED_PROBES_CGROUP_ANDROID_CGROUPS_H_
#define SRC_TRACED_PROBES_CGROUP_ANDROID_CGROUPS_H_

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/config/linux/cgroup_config.pbzero.h"

namespace perfetto {

inline constexpr uint32_t kAndroidUserOffset = 100000;

// A cgroup in the hierarchy created by Android's libprocessgroup:
//   <root>/{apps,system}                      LEVEL_TOP
//   <root>/{apps,system}/uid_<uid>            LEVEL_UID
//   <root>/{apps,system}/uid_<uid>/pid_<pid>  LEVEL_PID
struct AndroidCgroup {
  using Group = protos::pbzero::MemcgConfig::AndroidScope::Group;
  using Level = protos::pbzero::MemcgConfig::AndroidScope::Level;

  Level level = Level::LEVEL_UNSPECIFIED;
  Group group = Group::GROUP_UNSPECIFIED;
  // Only meaningful at uid or pid level.
  uint32_t uid = 0;
  // Only meaningful at pid level.
  uint32_t pid = 0;

  auto AsTuple() const { return std::tie(level, group, uid, pid); }

  bool operator==(const AndroidCgroup& other) const {
    return AsTuple() == other.AsTuple();
  }

  template <typename H>
  friend H PerfettoHashValue(H h, const AndroidCgroup& cgroup) {
    return std::apply(
        [&h](const auto&... fields) {
          return H::Combine(std::move(h), fields...);
        },
        cgroup.AsTuple());
  }
};

// A validated MemcgConfig.AndroidScope. The filters that are set are AND'd,
// and the values within a filter are OR'd. An unset filter matches everything.
struct AndroidCgroupScope {
  using Group = AndroidCgroup::Group;
  using Level = AndroidCgroup::Level;

  static std::optional<AndroidCgroupScope> Create(
      protozero::ConstBytes scope_pb,
      const base::FlatHashMap<std::string, uint32_t>& package_app_ids);

  // Matches all cgroups at the uid level. This is the default if a
  // MemcgConfig has no scopes.
  static AndroidCgroupScope Default();

  bool HasLevel(Level level) const { return levels.count(level) > 0; }
  bool MatchesGroup(Group group) const {
    return groups.empty() || groups.count(group) > 0;
  }
  bool MatchesUid(uint32_t uid) const {
    return (uids.empty() || uids.count(uid) > 0) &&
           (!app_ids || app_ids->count(uid % kAndroidUserOffset) > 0);
  }
  bool MatchesPid(uint32_t pid, const std::string& cmdline) const;

  // Whether any of the filters on the uid (or pid) of a cgroup is set.
  bool HasUidFilter() const { return !uids.empty() || app_ids.has_value(); }
  bool HasPidFilter() const {
    return !pids.empty() || !cmdline_patterns.empty();
  }

  // The levels at which the matching cgroups are recorded.
  base::FlatSet<Level> levels;

  // The filters.
  base::FlatSet<Group> groups;
  base::FlatSet<uint32_t> uids;
  // Resolved from the package_name filter: the app ids of the installed
  // packages. Optional because we need to distinguish "no matching packages
  // installed" from "no packages specified". Separate from |uids| because we
  // match these across android users/profiles, while |uids| are exact.
  std::optional<base::FlatSet<uint32_t>> app_ids;
  base::FlatSet<uint32_t> pids;
  std::vector<std::string> cmdline_patterns;
};

// Reads the /proc/pid/cmdline of a process.
using ReadCmdlineFn = std::function<bool(uint32_t pid, std::string* cmdline)>;

// Called with a selected cgroup and its absolute path.
using AndroidCgroupVisitor =
    std::function<void(const AndroidCgroup& cgroup, const std::string& path)>;

// Walks the group -> uid -> pid directory tree under |root|, calling |visit|
// for each cgroup that matches any of the |scopes|.
void VisitSelectedCgroups(const std::string& root,
                          const std::vector<AndroidCgroupScope>& scopes,
                          const ReadCmdlineFn& read_cmdline,
                          const AndroidCgroupVisitor& visit);

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_CGROUP_ANDROID_CGROUPS_H_
