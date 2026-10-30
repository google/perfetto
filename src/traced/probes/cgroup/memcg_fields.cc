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

#include "src/traced/probes/cgroup/memcg_fields.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/message.h"
#include "protos/perfetto/trace/linux/memcg_stats.pbzero.h"

namespace perfetto {
namespace {

using MC = protos::pbzero::MemcgConfig;
using MemcgField = protos::pbzero::MemcgConfig_MemcgField;

static_assert(std::size(kMemcgFieldDefs) == kMemcgFieldCount);

constexpr bool FieldDefsAreDense() {
  for (size_t i = 0; i < std::size(kMemcgFieldDefs); i++) {
    if (static_cast<size_t>(kMemcgFieldDefs[i].field) != i)
      return false;
  }
  return true;
}
static_assert(FieldDefsAreDense(), "the table must be in enum order");

const MemcgFieldDef* GetFieldDef(MemcgField field) {
  auto idx = static_cast<size_t>(field);
  if (idx == 0 || idx >= kMemcgFieldCount)
    return nullptr;
  return &kMemcgFieldDefs[idx];
}

// True for files containing "<key> <value>" lines.
bool IsKeyedMemcgFile(MemcgFile file) {
  return kMemcgFileDefs[static_cast<size_t>(file)].nested_proto_field != 0;
}

// True if this is a convenience _ALL enum that expands to the entire keyed
// file.
bool IsAllField(const MemcgFieldDef& def) {
  return IsKeyedMemcgFile(def.file) && def.key.empty();
}

// The field set for this file.
const MemcgFieldSet& MemcgFieldsInFile(MemcgFile file) {
  // Cannot be constexpr until C++23.
  static const std::array<MemcgFieldSet, kMemcgFileCount> fields_in_file = [] {
    std::array<MemcgFieldSet, kMemcgFileCount> ret;
    for (const MemcgFieldDef& def : kMemcgFieldDefs) {
      if (def.field != MC::MEMCG_UNSPECIFIED && !IsAllField(def))
        ret[static_cast<size_t>(def.file)].set(static_cast<size_t>(def.field));
    }
    return ret;
  }();
  return fields_in_file[static_cast<size_t>(file)];
}

// Note: this is a linear scan of the lookup table, and will be called for each
// line in a keyed file, however the performance is acceptable and keeps the
// code simple.
std::optional<MemcgField> FieldForKey(MemcgFile file, std::string_view key) {
  const MemcgFieldSet& fields = MemcgFieldsInFile(file);
  for (size_t i = 0; i < kMemcgFieldCount; i++) {
    if (fields.test(i) && kMemcgFieldDefs[i].key == key)
      return static_cast<MemcgField>(i);
  }
  return std::nullopt;
}

// Parses the contents of a single-valued file.
std::optional<uint64_t> ParseMemcgSingleValue(std::string_view sv) {
  while (!sv.empty() && (sv.back() == '\n' || sv.back() == ' ')) {
    sv.remove_suffix(1);
  }
  // Limit files such as memory.high return "max" for no limit.
  if (sv == "max")
    return kMemcgValueMax;
  return base::StringViewToNumber<uint64_t>(sv);
}

// Parses the contents of a keyed |file|, recording the values of the keys in
// |wanted| into |out|.
void ParseMemcgKeyedFile(MemcgFile file,
                         std::string_view contents,
                         const MemcgFieldSet& wanted,
                         MemcgValues* out) {
  PERFETTO_DCHECK(IsKeyedMemcgFile(file));
  MemcgFieldSet remaining = wanted & MemcgFieldsInFile(file);
  while (remaining.any()) {
    size_t eol_pos = contents.find('\n');
    if (eol_pos == std::string_view::npos)
      break;
    std::string_view line = contents.substr(0, eol_pos);
    contents.remove_prefix(eol_pos + 1);  // advance past the line

    size_t space_pos = line.find(' ');
    if (space_pos == std::string_view::npos)
      continue;
    std::optional<MemcgField> field =
        FieldForKey(file, line.substr(0, space_pos));
    if (!field || !remaining.test(static_cast<size_t>(*field)))
      continue;
    std::optional<uint64_t> value =
        base::StringViewToNumber<uint64_t>(line.substr(space_pos + 1));
    if (!value.has_value())
      continue;

    out->Set(*field, *value);
    remaining.reset(static_cast<size_t>(*field));
  }
}

}  // namespace

const char* MemcgFileName(MemcgFile file) {
  return kMemcgFileDefs[static_cast<size_t>(file)].name;
}

MemcgFieldSet ToMemcgFieldSet(const std::vector<MemcgField>& fields) {
  MemcgFieldSet ret;
  for (MemcgField field : fields) {
    const MemcgFieldDef* def = GetFieldDef(field);
    if (!def) {
      PERFETTO_DLOG("Ignoring unknown memcg field %d", static_cast<int>(field));
      continue;
    }
    if (IsAllField(*def)) {
      ret |= MemcgFieldsInFile(def->file);
    } else {
      ret.set(static_cast<size_t>(field));
    }
  }
  return ret;
}

MemcgFileSet MemcgFilesForFields(const MemcgFieldSet& fields) {
  MemcgFileSet ret;
  for (size_t i = 0; i < kMemcgFileCount; i++) {
    if ((fields & MemcgFieldsInFile(static_cast<MemcgFile>(i))).any())
      ret.set(i);
  }
  return ret;
}

void ParseMemcgFile(MemcgFile file,
                    std::string_view contents,
                    const MemcgFieldSet& wanted,
                    MemcgValues* out) {
  if (IsKeyedMemcgFile(file)) {
    ParseMemcgKeyedFile(file, contents, wanted, out);
    return;
  }
  // Single-valued files host one field.
  MemcgFieldSet fields = wanted & MemcgFieldsInFile(file);
  std::optional<uint64_t> value = ParseMemcgSingleValue(contents);
  if (fields.none() || !value.has_value())
    return;
  for (size_t i = 0; i < fields.size(); i++) {
    if (fields.test(i)) {
      out->Set(static_cast<MemcgField>(i), *value);
      break;
    }
  }
}

void SerializeMemcgValues(const MemcgValues& values,
                          protos::pbzero::MemcgStats_Cgroup* out) {
  for (const MemcgFileDef& file_def : kMemcgFileDefs) {
    MemcgFieldSet present = values.present() & MemcgFieldsInFile(file_def.file);
    if (present.none())
      continue;
    // Open a nested message if this is a keyed file.
    protozero::Message* msg = out;
    if (IsKeyedMemcgFile(file_def.file))
      msg = out->BeginNestedMessage<protozero::Message>(
          file_def.nested_proto_field);
    for (size_t i = 0; i < kMemcgFieldCount; i++) {
      if (!present.test(i))
        continue;
      msg->AppendVarInt(kMemcgFieldDefs[i].proto_field,
                        *values.Get(static_cast<MemcgField>(i)));
    }
  }
}

}  // namespace perfetto
