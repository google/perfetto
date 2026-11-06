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

#ifndef SRC_TRACED_PROBES_CGROUP_MEMCG_READER_H_
#define SRC_TRACED_PROBES_CGROUP_MEMCG_READER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/protozero/field.h"
#include "src/traced/probes/cgroup/android_cgroups.h"
#include "src/traced/probes/cgroup/memcg_fields.h"

namespace perfetto {

namespace protos::pbzero {
class MemcgStats;
}  // namespace protos::pbzero

// Reads and serialises cgroup memory controller (memcg) accounting.
class MemcgReader {
 public:
  // A validated MemcgConfig.
  struct Config {
    // |config_pb| is a serialized MemcgConfig. If it has package_name filters,
    // they are resolved here, by reading |packages_list_path| once.
    static std::optional<Config> Create(
        protozero::ConstBytes config_pb,
        const std::string& packages_list_path = "/data/system/packages.list");

    // Fields to record, spanning both single-valued and keyed files.
    MemcgFieldSet fields;
    // Files that need reading for |fields|.
    MemcgFileSet files;
    // Which cgroups should be recorded.
    std::vector<AndroidCgroupScope> scopes;
    // If zero: read once at the start.
    uint32_t poll_period_ms = 0;
    // If caching to avoid emitting unchanged values, emit absolute snapshots
    // after this many read calls. Zero if not caching.
    uint32_t cache_ttl_polls = 0;
  };

  explicit MemcgReader(Config config,
                       std::string cgroup_root = "/sys/fs/cgroup");

  // Finds the cgroups in scope, reads the values, and serialises
  // MemcgStats.Cgroup messages into the message returned by |lazy_out|.
  // |lazy_out| must return the same message every time. It's only called if
  // there are values to record, so it can create the message on first use.
  void Read(const std::function<protos::pbzero::MemcgStats*()>& lazy_out);

  const Config& config() const { return config_; }

 private:
  // The last recorded values of a cgroup for caching: one per field in
  // |config_.fields|, in field order, or nullopt if not recorded yet.
  using CachedValues = std::vector<std::optional<uint64_t>>;

  // Reads the files of the cgroup at |path|, using |path_buf| and |file_buf|
  // as scratch space.
  MemcgValues ReadCgroup(const std::string& path,
                         std::string* path_buf,
                         std::string* file_buf);

  // Records |values| as the last values of |cgroup|, and returns the ones that
  // changed.
  MemcgValues UpdateCache(const AndroidCgroup& cgroup,
                          const MemcgValues& values);

  const Config config_;
  const std::string cgroup_root_;
  // The cgroups recorded since the cache was last emptied, if caching.
  // Note: unordered_map over base::FlatHashMap because we want to only allocate
  // as much as needed, and lookup speed doesn't matter.
  std::unordered_map<AndroidCgroup,
                     CachedValues,
                     base::MurmurHash<AndroidCgroup>>
      cache_;
  // Number of times Read() was called.
  uint64_t read_count_ = 0;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_CGROUP_MEMCG_READER_H_
