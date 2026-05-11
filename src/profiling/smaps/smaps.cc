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

#include "perfetto/ext/profiling/smaps.h"

#include <stdio.h>

#include <deque>
#include <string_view>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "protos/perfetto/trace/profiling/smaps.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace profiling {
namespace {

class StringInterner {
 public:
  using StringId = size_t;

  StringInterner() {
    // index zero is always the empty string
    Intern(std::string_view{});
  }

  StringId Intern(std::string_view s) {
    if (auto* p = map_.Find(s); p) {
      return *p;
    }
    size_t index = storage_.size();
    storage_.emplace_back(s);
    std::string_view stable_sv(storage_.back());
    map_.Insert(stable_sv, index);
    return index;
  }

  const std::deque<std::string>& OrderedStrings() const { return storage_; }

 private:
  base::FlatHashMap<std::string_view, StringId> map_;
  std::deque<std::string> storage_;
};

struct Vma {
  StringInterner::StringId name_id = 0;
  uint32_t aggregate_count = 1;

  uint64_t size_kb = 0;
  uint64_t rss_kb = 0;
  uint64_t anonymous_kb = 0;
  uint64_t swap_kb = 0;
  uint64_t shared_clean_kb = 0;
  uint64_t shared_dirty_kb = 0;
  uint64_t private_clean_kb = 0;
  uint64_t private_dirty_kb = 0;
  uint64_t locked_kb = 0;
  uint64_t pss_kb = 0;
  uint64_t pss_dirty_kb = 0;
  uint64_t swap_pss_kb = 0;
};

// clang-format off
enum SmapsField : uint32_t {
  kSize         = 1 << 0,
  kRss          = 1 << 1,
  kAnonymous    = 1 << 2,
  kSwap         = 1 << 3,
  kSharedClean  = 1 << 4,
  kSharedDirty  = 1 << 5,
  kPrivateClean = 1 << 6,
  kPrivateDirty = 1 << 7,
  kLocked       = 1 << 8,
  kPss          = 1 << 9,
  kPssDirty     = 1 << 10,
  kSwapPss      = 1 << 11,
};
// clang-format on

// Convenience mapping between config proto enums, implementation bitflags,
// field offsets, and trace proto field ids.
struct SmapsFieldDef {
  SmapsField flag;
  uint64_t Vma::* member_ptr;
  int32_t config_pb_enum;
  uint32_t trace_field_id;
};
using SC = protos::gen::SmapsConfig;
using SP = protos::pbzero::PackedSmaps;
// clang-format off
constexpr SmapsFieldDef kSmapsFieldDefs[] = {
  {kSize,         &Vma::size_kb,          SC::VMA_FIELD_SIZE,          SP::kSizeKbFieldNumber},
  {kRss,          &Vma::rss_kb,           SC::VMA_FIELD_RSS,           SP::kRssKbFieldNumber},
  {kAnonymous,    &Vma::anonymous_kb,     SC::VMA_FIELD_ANONYMOUS,     SP::kAnonymousKbFieldNumber},
  {kSwap,         &Vma::swap_kb,          SC::VMA_FIELD_SWAP,          SP::kSwapKbFieldNumber},
  {kSharedClean,  &Vma::shared_clean_kb,  SC::VMA_FIELD_SHARED_CLEAN,  SP::kSharedCleanKbFieldNumber},
  {kSharedDirty,  &Vma::shared_dirty_kb,  SC::VMA_FIELD_SHARED_DIRTY,  SP::kSharedDirtyKbFieldNumber},
  {kPrivateClean, &Vma::private_clean_kb, SC::VMA_FIELD_PRIVATE_CLEAN, SP::kPrivateCleanKbFieldNumber},
  {kPrivateDirty, &Vma::private_dirty_kb, SC::VMA_FIELD_PRIVATE_DIRTY, SP::kPrivateDirtyKbFieldNumber},
  {kLocked,       &Vma::locked_kb,        SC::VMA_FIELD_LOCKED,        SP::kLockedKbFieldNumber},
  {kPss,          &Vma::pss_kb,           SC::VMA_FIELD_PSS,           SP::kPssKbFieldNumber},
  {kPssDirty,     &Vma::pss_dirty_kb,     SC::VMA_FIELD_PSS_DIRTY,     SP::kPssDirtyKbFieldNumber},
  {kSwapPss,      &Vma::swap_pss_kb,      SC::VMA_FIELD_SWAP_PSS,      SP::kSwapPssKbFieldNumber},
};
// clang-format on

void AggregateVma(Vma& dest, const Vma& src) {
  dest.aggregate_count += src.aggregate_count;

  for (const auto& field : kSmapsFieldDefs) {
    dest.*(field.member_ptr) += src.*(field.member_ptr);
  }
}

std::string_view ExtractMappingName(std::string_view line) {
  // Skip until the last space-delimited column, which can itself contain spaces
  // so we can't tokenise from the end.
  size_t pos = 0;
  for (int i = 0; i < 5; ++i) {
    if (pos = line.find(' ', pos); pos == std::string_view::npos)
      return {};
    if (pos = line.find_first_not_of(' ', pos); pos == std::string_view::npos)
      return {};
  }

  size_t end = line.size() - 1;  // loop above guarantees size > 0
  if (pos >= end)
    return {};
  return line.substr(pos, end - pos);
}

void ParseSmapsLine(const char* line, Vma& vma, uint32_t& fields) {
  if (!line)
    return;

  // Note: strtoull skips leading spaces and the "kB" suffix. This is not
  // interchangeable with base::CStringToUInt64.
  switch (line[0]) {
    case 'S': {
      if ((fields & kSize) && strncmp(line, "Size:", 5) == 0) {
        vma.size_kb = std::strtoull(line + 5, nullptr, 10);
        fields &= ~kSize;
      } else if ((fields & kSwap) && strncmp(line, "Swap:", 5) == 0) {
        vma.swap_kb = std::strtoull(line + 5, nullptr, 10);
        fields &= ~kSwap;
      } else if ((fields & kSwapPss) && strncmp(line, "SwapPss:", 8) == 0) {
        vma.swap_pss_kb = std::strtoull(line + 8, nullptr, 10);
        fields &= ~kSwapPss;
      } else if ((fields & kSharedClean) &&
                 strncmp(line, "Shared_Clean:", 13) == 0) {
        vma.shared_clean_kb = std::strtoull(line + 13, nullptr, 10);
        fields &= ~kSharedClean;
      } else if ((fields & kSharedDirty) &&
                 strncmp(line, "Shared_Dirty:", 13) == 0) {
        vma.shared_dirty_kb = std::strtoull(line + 13, nullptr, 10);
        fields &= ~kSharedDirty;
      }
      break;
    }
    case 'R': {
      if ((fields & kRss) && strncmp(line, "Rss:", 4) == 0) {
        vma.rss_kb = std::strtoull(line + 4, nullptr, 10);
        fields &= ~kRss;
      }
      break;
    }
    case 'A': {
      if ((fields & kAnonymous) && strncmp(line, "Anonymous:", 10) == 0) {
        vma.anonymous_kb = std::strtoull(line + 10, nullptr, 10);
        fields &= ~kAnonymous;
      }
      break;
    }
    case 'P': {
      if ((fields & kPss) && strncmp(line, "Pss:", 4) == 0) {
        vma.pss_kb = std::strtoull(line + 4, nullptr, 10);
        fields &= ~kPss;
      } else if ((fields & kPssDirty) && strncmp(line, "Pss_Dirty:", 10) == 0) {
        vma.pss_dirty_kb = std::strtoull(line + 10, nullptr, 10);
        fields &= ~kPssDirty;
      } else if ((fields & kPrivateClean) &&
                 strncmp(line, "Private_Clean:", 14) == 0) {
        vma.private_clean_kb = std::strtoull(line + 14, nullptr, 10);
        fields &= ~kPrivateClean;
      } else if ((fields & kPrivateDirty) &&
                 strncmp(line, "Private_Dirty:", 14) == 0) {
        vma.private_dirty_kb = std::strtoull(line + 14, nullptr, 10);
        fields &= ~kPrivateDirty;
      }
      break;
    }
    case 'L': {
      if ((fields & kLocked) && strncmp(line, "Locked:", 7) == 0) {
        vma.locked_kb = std::strtoull(line + 7, nullptr, 10);
        fields &= ~kLocked;
      }
      break;
    }
    default:
      break;
  }
}

template <typename FN>
void Parse(FILE* file,
           StringInterner& interner,
           uint32_t requested_fields,
           FN callback) {
  Vma vma = Vma{};
  bool in_vma = false;
  // bitmask of the fields that still need to be parsed for the current vma
  uint32_t fields_to_parse = 0;

  // getline (re)allocates the buffer, so free it when done
  char* buf = nullptr;
  auto getline_cleanup = base::OnScopeExit([&] { free(buf); });

  size_t buf_len = 0;
  ssize_t read_len = 0;
  while ((read_len = getline(&buf, &buf_len, file)) != -1) {
    std::string_view line(buf, static_cast<size_t>(read_len));
    if (line.empty())
      continue;

    // Test if we're at a new vma boundary by checking that this isn't a
    // colon-delimited line. Example of the two types of line:
    // 7f13720e6000-7f13720e8000 r-xp 00000000 00:00 0            [vdso]
    // Size:                  8 kB
    // Rss:                   8 kB
    size_t space_pos = line.find(' ');
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string_view::npos || space_pos < colon_pos) {
      if (in_vma) {
        callback(vma);
      }

      vma = Vma{};
      vma.name_id = interner.Intern(ExtractMappingName(line));
      in_vma = true;
      fields_to_parse = requested_fields;

    } else if (in_vma) {
      if (!fields_to_parse) {
        continue;  // done, skip until the next vma
      }
      ParseSmapsLine(buf, vma, fields_to_parse);
    }
  }

  if (in_vma) {
    callback(vma);
  }
}
}  // namespace

void ParseAndSerializeSmaps(FILE* file,
                            const protos::gen::SmapsConfig& config,
                            protos::pbzero::SmapsPacket* packet) {
  if (!file || !packet)
    return;

  // Config -> bitmask of fields to collect.
  uint32_t parser_mask = kSize | kRss | kAnonymous | kSwap;
  if (config.vma_fields_size()) {
    parser_mask = 0;
    for (int32_t pb_enum : config.vma_fields()) {
      for (const auto& def : kSmapsFieldDefs) {
        if (def.config_pb_enum == pb_enum) {
          parser_mask |= def.flag;
        }
      }
    }
  }
  bool aggregated = !config.unaggregated();

  // Parse the file:

  StringInterner interner;
  std::vector<Vma> vmas;
  // If we're aggregating by name, use the vector as a map with the interned
  // name as the index (since the StringInterner assigns ids in a sequential
  // order).
  // So since the interner always assigns the empty string the id 0,
  // pre-create that vector entry.
  if (aggregated) {
    vmas.push_back(Vma{});
    vmas[0].aggregate_count = 0;
  }

  Parse(file, interner, parser_mask, [&vmas, aggregated](Vma vma) {
    if (!aggregated) {
      vmas.push_back(vma);
      return;
    }
    // aggregated: index into vector with interned id.
    size_t name_id = vma.name_id;
    if (name_id < vmas.size()) {
      AggregateVma(vmas[name_id], vma);
    } else {
      vmas.resize(name_id + 1);
      vmas[name_id] = vma;
    }
  });

  // Serialise the proto:

  auto packed_smaps = packet->set_packed_entries();
  // string_table
  for (const auto& v : interner.OrderedStrings()) {
    packed_smaps->add_string_table(v);
  }

  protozero::PackedVarInt packed;
  // If aggregating: write aggregate_count, but skip name_id as a size
  // optimisation. We write the vmas exactly in string_table order, so the
  // serialised name_id would be 0, 1, 2, 3, ...
  if (aggregated) {
    packed.Reset();
    for (const auto& vma : vmas) {
      packed.Append(vma.aggregate_count);
    }
    packed_smaps->set_aggregate_count(packed);
  } else {
    // Unaggregated: write name_id.
    for (const auto& vma : vmas) {
      packed.Append(static_cast<uint32_t>(vma.name_id));
    }
    packed_smaps->set_name_id(packed);
  }

  // write value fields
  for (const auto& field : kSmapsFieldDefs) {
    if (parser_mask & field.flag) {
      packed.Reset();
      for (const auto& vma : vmas) {
        packed.Append(vma.*(field.member_ptr));
      }
      packed_smaps->AppendBytes(field.trace_field_id, packed.data(),
                                packed.size());
    }
  }
}

}  // namespace profiling
}  // namespace perfetto
