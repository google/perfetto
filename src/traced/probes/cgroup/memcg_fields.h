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

#ifndef SRC_TRACED_PROBES_CGROUP_MEMCG_FIELDS_H_
#define SRC_TRACED_PROBES_CGROUP_MEMCG_FIELDS_H_

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "perfetto/ext/traced/memcg_field_defs.h"
#include "protos/perfetto/config/linux/cgroup_config.pbzero.h"

namespace perfetto {

namespace protos::pbzero {
class MemcgStats_Cgroup;
}  // namespace protos::pbzero

// This header and the cc file handle mapping of MemcgConfig.MemcgField enums
// onto bitmasks representing files and fields, which are later used to parse
// and serialise the cgroup values.

inline constexpr size_t kMemcgFileCount =
    static_cast<size_t>(MemcgFile::kNumFiles);
inline constexpr size_t kMemcgFieldCount =
    static_cast<size_t>(protos::pbzero::MemcgConfig_MemcgField_MAX) + 1;

using MemcgFileSet = std::bitset<kMemcgFileCount>;
using MemcgFieldSet = std::bitset<kMemcgFieldCount>;

// Value used for the "max" special value of the limit files (e.g. memory.high).
// The kernel never emits this as a number.
inline constexpr uint64_t kMemcgValueMax = std::numeric_limits<uint64_t>::max();

// Values read from a single cgroup.
class MemcgValues {
 public:
  void Set(protos::pbzero::MemcgConfig_MemcgField field, uint64_t value) {
    auto idx = static_cast<size_t>(field);
    present_.set(idx);
    values_[idx] = value;
  }

  std::optional<uint64_t> Get(
      protos::pbzero::MemcgConfig_MemcgField field) const {
    auto idx = static_cast<size_t>(field);
    if (!present_.test(idx))
      return std::nullopt;
    return values_[idx];
  }

  void Clear() { present_.reset(); }

  const MemcgFieldSet& present() const { return present_; }

 private:
  MemcgFieldSet present_;
  std::array<uint64_t, kMemcgFieldCount> values_{};
};

// File name within the cgroup directory.
const char* MemcgFileName(MemcgFile file);

// Converts the config's list of field enums into a set, expanding the _ALL
// values and dropping unknowns.
MemcgFieldSet ToMemcgFieldSet(
    const std::vector<protos::pbzero::MemcgConfig_MemcgField>& fields);

// The files that need to be read to record |fields|.
MemcgFileSet MemcgFilesForFields(const MemcgFieldSet& fields);

// Parses the contents of |file|, single-valued or keyed, recording the values
// of the fields in |wanted| into |out|.
void ParseMemcgFile(MemcgFile file,
                    std::string_view contents,
                    const MemcgFieldSet& wanted,
                    MemcgValues* out);

void SerializeMemcgValues(const MemcgValues& values,
                          protos::pbzero::MemcgStats_Cgroup* out);

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_CGROUP_MEMCG_FIELDS_H_
