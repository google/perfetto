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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_splitter.h"
#include "protos/perfetto/config/linux/cgroup_config.pbzero.h"
#include "protos/perfetto/trace/linux/memcg_stats.pbzero.h"
#include "src/profiling/common/proc_cmdline.h"
#include "src/traced/probes/packages_list/packages_list_parser.h"

namespace perfetto {
namespace {

using MemcgField = protos::pbzero::MemcgConfig::MemcgField;

constexpr uint32_t kMinPollPeriodMs = 100;

// Recorded if a MemcgConfig doesn't list any fields.
constexpr MemcgField kDefaultFields[] = {
    protos::pbzero::MemcgConfig::MEMCG_CURRENT,
    protos::pbzero::MemcgConfig::MEMCG_STAT_ANON,
    protos::pbzero::MemcgConfig::MEMCG_STAT_FILE,
};

protos::pbzero::MemcgStats::Group ToProtoGroup(AndroidCgroup::Group group) {
  using ProtoGroup = protos::pbzero::MemcgStats::Group;
  switch (group) {
    case AndroidCgroup::Group::GROUP_APPS:
      return ProtoGroup::GROUP_APPS;
    case AndroidCgroup::Group::GROUP_SYSTEM:
      return ProtoGroup::GROUP_SYSTEM;
    case AndroidCgroup::Group::GROUP_UNSPECIFIED:
      break;
  }
  return ProtoGroup::GROUP_UNSPECIFIED;
}

// Reads packages.list into a package name -> app id map.
base::FlatHashMap<std::string, uint32_t> ReadPackageAppIds(
    const std::string& path) {
  base::FlatHashMap<std::string, uint32_t> app_ids;
  std::string contents;
  if (!base::ReadFile(path, &contents)) {
    PERFETTO_PLOG("linux.cgroup: failed to read %s", path.c_str());
    return app_ids;
  }
  for (base::StringSplitter ss(std::move(contents), '\n'); ss.Next();) {
    Package package;
    if (!ReadPackagesListLine(ss.cur_token(), &package))
      continue;
    app_ids.Insert(std::move(package.name), static_cast<uint32_t>(package.uid));
  }
  return app_ids;
}

}  // namespace

// static
std::optional<MemcgReader::Config> MemcgReader::Config::Create(
    protozero::ConstBytes config_bytes,
    const std::string& packages_list_path) {
  protos::pbzero::MemcgConfig::Decoder config_pb(config_bytes);
  Config config;

  // Flattened enum -> sets of files and fields.
  std::vector<MemcgField> field_enums;
  for (auto it = config_pb.field(); it; ++it)
    field_enums.push_back(static_cast<MemcgField>(*it));
  // If no fields specified, use a default list.
  if (field_enums.empty())
    field_enums.assign(std::begin(kDefaultFields), std::end(kDefaultFields));
  config.fields = ToMemcgFieldSet(field_enums);
  if (config.fields.none()) {
    PERFETTO_ELOG("linux.cgroup: memcg_config has no known fields");
    return std::nullopt;
  }
  config.files = MemcgFilesForFields(config.fields);

  // Read packages.list only if a scope has a package_name filter.
  bool has_package_filter = false;
  for (auto it = config_pb.android_scope(); it && !has_package_filter; ++it) {
    protos::pbzero::MemcgConfig::AndroidScope::Decoder scope_pb(*it);
    has_package_filter = scope_pb.has_package_name();
  }
  base::FlatHashMap<std::string, uint32_t> package_app_ids;
  if (has_package_filter)
    package_app_ids = ReadPackageAppIds(packages_list_path);

  // Scopes.
  for (auto it = config_pb.android_scope(); it; ++it) {
    std::optional<AndroidCgroupScope> scope =
        AndroidCgroupScope::Create(*it, package_app_ids);
    if (!scope.has_value())
      return std::nullopt;
    config.scopes.push_back(std::move(*scope));
  }
  if (config.scopes.empty())
    config.scopes.push_back(AndroidCgroupScope::Default());

  // Poll period of zero means one-shot.
  if (config_pb.poll_period_ms() > 0) {
    config.poll_period_ms =
        std::max(config_pb.poll_period_ms(), kMinPollPeriodMs);
    if (config.poll_period_ms != config_pb.poll_period_ms()) {
      PERFETTO_ILOG("linux.cgroup: clamped poll_period_ms from %u to %u",
                    config_pb.poll_period_ms(), config.poll_period_ms);
    }
    // Rounded down to a multiple of the poll period. Not caching if shorter
    // than two polls, since every read would record all values anyway.
    uint32_t ttl_polls = config_pb.cache_ttl_ms() / config.poll_period_ms;
    config.cache_ttl_polls = ttl_polls > 1 ? ttl_polls : 0;
  }
  return config;
}

MemcgReader::MemcgReader(Config config, std::string cgroup_root)
    : config_(std::move(config)), cgroup_root_(std::move(cgroup_root)) {}

void MemcgReader::Read(
    const std::function<protos::pbzero::MemcgStats*()>& lazy_out) {
  bool caching = config_.cache_ttl_polls > 0;
  // Every |cache_ttl_polls| reads, start without recorded values, so that
  // all values get recorded.
  if (caching && read_count_ % config_.cache_ttl_polls == 0)
    cache_.clear();
  read_count_++;

  auto read_cmdline = [](uint32_t pid, std::string* cmdline) {
    return profiling::glob_aware::ReadProcCmdlineForPID(static_cast<pid_t>(pid),
                                                        cmdline);
  };
  // Reused across the cgroups of this read, to avoid allocations.
  std::string path_buf;
  std::string file_buf;
  VisitSelectedCgroups(
      cgroup_root_, config_.scopes, read_cmdline,
      [&](const AndroidCgroup& cgroup, const std::string& path) {
        MemcgValues values = ReadCgroup(path, &path_buf, &file_buf);
        if (caching)
          values = UpdateCache(cgroup, values);
        if (values.present().none())
          return;
        auto* cgroup_pb = lazy_out()->add_cgroup();
        cgroup_pb->set_android_group(ToProtoGroup(cgroup.group));
        if (cgroup.level != AndroidCgroup::Level::LEVEL_TOP)
          cgroup_pb->set_uid(cgroup.uid);
        if (cgroup.level == AndroidCgroup::Level::LEVEL_PID)
          cgroup_pb->set_pid(cgroup.pid);
        SerializeMemcgValues(values, cgroup_pb);
      });
}

MemcgValues MemcgReader::ReadCgroup(const std::string& path,
                                    std::string* path_buf,
                                    std::string* file_buf) {
  MemcgValues values;
  for (size_t i = 0; i < kMemcgFileCount; i++) {
    if (!config_.files.test(i))
      continue;
    auto file = static_cast<MemcgFile>(i);
    path_buf->assign(path).append("/").append(MemcgFileName(file));
    file_buf->clear();
    if (!base::ReadFile(*path_buf, file_buf))
      continue;
    ParseMemcgFile(file, *file_buf, config_.fields, &values);
  }
  return values;
}

MemcgValues MemcgReader::UpdateCache(const AndroidCgroup& cgroup,
                                     const MemcgValues& values) {
  MemcgValues changed;
  if (values.present().none())
    return changed;

  // Create the initial cache for this cgroup (sized according to number of
  // fields being recorded), or find the existing entry.
  size_t field_count = config_.fields.count();
  auto it = cache_.try_emplace(cgroup, field_count).first;
  CachedValues& cached = it->second;

  // Update the vector and return the fields that have changed.
  size_t vec_slot = 0;
  for (size_t i = 0; i < kMemcgFieldCount; i++) {
    if (!config_.fields.test(i))
      continue;
    MemcgField field = static_cast<MemcgField>(i);
    std::optional<uint64_t>& last = cached[vec_slot++];
    std::optional<uint64_t> value = values.Get(field);
    if (!value.has_value() || value == last)
      continue;
    changed.Set(field, *value);
    last = value;
  }
  return changed;
}

}  // namespace perfetto
