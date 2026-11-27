/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/common/args_tracker.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

ArgsTracker::ArgsTracker(TraceStorage* storage) : storage_(storage) {}

ArgsInserter ArgsTracker::AddArgsTo(MetadataId id) {
  return Bind(
      storage_->mutable_metadata_table(),
      [](void* writer, uint32_t row, ArgSetId set_id) {
        static_cast<tables::MetadataTable*>(writer)
            ->SetCell<tables::MetadataTable::ColumnIndex::int_value>(
                row, static_cast<int64_t>(set_id));
      },
      id.value, id.value);
}

ArgsInserter ArgsTracker::AddArgsTo(TrackId id) {
  return Bind(
      storage_->mutable_track_table(),
      [](void* writer, uint32_t row, ArgSetId set_id) {
        static_cast<tables::TrackTable*>(writer)
            ->SetCell<tables::TrackTable::ColumnIndex::source_arg_set_id>(
                row, set_id);
      },
      id.value, id.value);
}

ArgsInserter ArgsTracker::AddArgsToProcess(UniquePid id) {
  return Bind(
      storage_->mutable_process_table(),
      [](void* writer, uint32_t row, ArgSetId set_id) {
        static_cast<tables::ProcessTable*>(writer)
            ->SetCell<tables::ProcessTable::ColumnIndex::arg_set_id>(row,
                                                                     set_id);
      },
      id, id);
}

ArgsInserter ArgsTracker::AddArgsToThread(UniqueTid id) {
  return Bind(
      storage_->mutable_thread_table(),
      [](void* writer, uint32_t row, ArgSetId set_id) {
        static_cast<tables::ThreadTable*>(writer)
            ->SetCell<tables::ThreadTable::ColumnIndex::arg_set_id>(row,
                                                                    set_id);
      },
      id, id);
}

ArgsInserter::ArgsInserter(ArgsTracker* tracker,
                           void* writer,
                           ArgsTracker::WriteArgSetIdFn write,
                           uint32_t row,
                           uint32_t id)
    : tracker_(tracker),
      buffer_(tracker->AcquireArgsBuffer()),
      writer_(writer),
      write_(write),
      row_(row),
      id_(id) {}

ArgsInserter::ArgsInserter(ArgsInserter&& other) noexcept
    : tracker_(other.tracker_),
      buffer_(other.buffer_),
      writer_(other.writer_),
      write_(other.write_),
      row_(other.row_),
      id_(other.id_) {
  other.tracker_ = nullptr;
  other.buffer_ = nullptr;
}

ArgsInserter& ArgsInserter::operator=(ArgsInserter&& other) noexcept {
  if (this != &other) {
    // Destroy our current state (commits + releases the buffer), then
    // move-construct from other in place.
    this->~ArgsInserter();
    new (this) ArgsInserter(std::move(other));
  }
  return *this;
}

ArgsInserter::~ArgsInserter() {
  if (!tracker_)  // Empty, moved-from, or a test mock: nothing to commit.
    return;
  Commit();
  tracker_->ReleaseArgsBuffer(buffer_);
}

ArgsInserter& ArgsInserter::AddArg(StringId flat_key,
                                   StringId key,
                                   Variadic value,
                                   UpdatePolicy update_policy) {
  std::vector<CompactArg>& args = buffer_->args;
  base::FlatHashMap<StringId, uint32_t>& key_index = buffer_->key_index;

  // Collapse same-key duplicates in place (kSkipIfExists keeps the first value,
  // kAddOrUpdate the last) so AddArgSet never sees two args with the same key.
  // Small sets use a linear scan; past a threshold we build `key_index` to keep
  // large sets (e.g. array args) O(n) rather than O(n^2). Empty index => still
  // scanning linearly.
  constexpr size_t kKeyIndexThreshold = 32;
  CompactArg* existing = nullptr;
  if (key_index.size() != 0) {
    if (uint32_t* idx = key_index.Find(key)) {
      existing = &args[*idx];
    }
  } else {
    for (CompactArg& arg : args) {
      if (arg.key == key) {
        existing = &arg;
        break;
      }
    }
  }
  if (existing) {
    if (update_policy == UpdatePolicy::kSkipIfExists) {
      return *this;
    }
    PERFETTO_DCHECK(update_policy == UpdatePolicy::kAddOrUpdate);
    existing->flat_key = flat_key;
    existing->value = value;
    existing->update_policy = update_policy;
    return *this;
  }

  auto new_index = static_cast<uint32_t>(args.size());
  args.emplace_back(CompactArg{flat_key, key, value, update_policy});
  if (key_index.size() != 0) {
    key_index.Insert(key, new_index);
  } else if (args.size() >= kKeyIndexThreshold) {
    for (uint32_t i = 0; i < args.size(); ++i) {
      key_index.Insert(args[i].key, i);
    }
  }
  return *this;
}

ArgsInserter::CompactArgSet ArgsInserter::ToCompactArgSet() && {
  CompactArgSet compact_args;
  for (const CompactArg& arg : buffer_->args) {
    compact_args.emplace_back(arg);
  }
  // Leave the buffer empty so Commit() is a no-op: the caller now owns the
  // args.
  buffer_->args.clear();
  buffer_->key_index.Clear();
  return compact_args;
}

void ArgsInserter::Commit() {
  std::vector<CompactArg>& args = buffer_->args;
  if (args.empty()) {
    return;
  }
  ArgSetId set_id =
      tracker_->AddArgSet(args.data(), static_cast<uint32_t>(args.size()));
  write_(writer_, row_, set_id);
}

uint64_t ArgsTracker::ComputeArgSetHash(const CompactArg* args,
                                        uint32_t count) {
  base::MurmurHashCombiner combiner;
  for (uint32_t i = 0; i < count; ++i) {
    const CompactArg& arg = args[i];
    combiner.Combine(arg.key);
    combiner.Combine(arg.value.type);
    // We don't hash arg.flat_key because it's a subsequence of arg.key.
    switch (arg.value.type) {
      case Variadic::Type::kInt:
        combiner.Combine(arg.value.int_value);
        break;
      case Variadic::Type::kUint:
        combiner.Combine(arg.value.uint_value);
        break;
      case Variadic::Type::kString:
        combiner.Combine(arg.value.string_value);
        break;
      case Variadic::Type::kReal:
        combiner.Combine(arg.value.real_value);
        break;
      case Variadic::Type::kPointer:
        combiner.Combine(arg.value.pointer_value);
        break;
      case Variadic::Type::kBool:
        combiner.Combine(arg.value.bool_value);
        break;
      case Variadic::Type::kJson:
        combiner.Combine(arg.value.json_value);
        break;
      case Variadic::Type::kNull:
        combiner.Combine(0);
        break;
    }
  }

  return combiner.digest();
}

ArgSetId ArgsTracker::AddArgSet(const CompactArg* args, uint32_t count) {
  auto& arg_table = *storage_->mutable_arg_table();

  uint32_t arg_set_id = arg_table.row_count();
  ArgSetHash digest = ComputeArgSetHash(args, count);
  auto [it, inserted] = arg_row_for_hash_.Insert(digest, arg_set_id);
  if (!inserted) {
    // Already inserted.
    return *it;
  }

  for (uint32_t i = 0; i < count; ++i) {
    const CompactArg& arg = args[i];
    tables::ArgTable::Row row;
    row.arg_set_id = arg_set_id;
    row.flat_key = arg.flat_key;
    row.key = arg.key;
    switch (arg.value.type) {
      case Variadic::Type::kInt:
        row.int_value = arg.value.int_value;
        break;
      case Variadic::Type::kUint:
        row.int_value = static_cast<int64_t>(arg.value.uint_value);
        break;
      case Variadic::Type::kString:
        row.string_value = arg.value.string_value;
        break;
      case Variadic::Type::kReal:
        row.real_value = arg.value.real_value;
        break;
      case Variadic::Type::kPointer:
        row.int_value = static_cast<int64_t>(arg.value.pointer_value);
        break;
      case Variadic::Type::kBool:
        row.int_value = arg.value.bool_value;
        break;
      case Variadic::Type::kJson:
        row.string_value = arg.value.json_value;
        break;
      case Variadic::Type::kNull:
        break;
    }
    row.value_type = storage_->GetIdForVariadicType(arg.value.type);
    arg_table.Insert(row);
  }
  return arg_set_id;
}

}  // namespace perfetto::trace_processor
