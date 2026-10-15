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
#include <optional>
#include <string>
#include <vector>

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
  };

  explicit MemcgReader(Config config,
                       std::string cgroup_root = "/sys/fs/cgroup");

  // Finds the cgroups in scope, reads the values, and serialises
  // MemcgStats.Cgroup messages into |out|.
  void Read(protos::pbzero::MemcgStats* out);

  const Config& config() const { return config_; }

 private:
  // Reads the files of the cgroup at |path|.
  MemcgValues ReadCgroup(const std::string& path);

  const Config config_;
  const std::string cgroup_root_;
  // Scratch buffer for reading files, avoids heap churn.
  std::string scratch_buf_;
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_CGROUP_MEMCG_READER_H_
