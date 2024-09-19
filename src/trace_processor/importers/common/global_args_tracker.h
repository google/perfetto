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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_GLOBAL_ARGS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_GLOBAL_ARGS_TRACKER_H_

#include <cstdint>
#include <type_traits>
#include <vector>
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto {
namespace trace_processor {

// Interns args into the storage from all ArgsTrackers across trace processor.
// Note: most users will want to use ArgsTracker to push args to the strorage
// and not this class. This class is really intended for ArgsTracker to use for
// that purpose.
class GlobalArgsTracker {
 public:
  // How to behave if two or more args with the same key were added into the
  // same ArgSet. If |kSkipIfExists|, the arg will be ignored if another arg
  // with the same key already exists. If |kAddOrUpdate|, any existing arg with
  // the same key will be overridden.
  enum class UpdatePolicy { kSkipIfExists, kAddOrUpdate };

  struct CompactArg {
    StringId flat_key = kNullStringId;
    StringId key = kNullStringId;
    Variadic value = Variadic::Integer(0);
    UpdatePolicy update_policy = UpdatePolicy::kAddOrUpdate;
  };
  static_assert(std::is_trivially_destructible<CompactArg>::value,
                "Args must be trivially destructible");

  struct Arg : public CompactArg {
    ColumnLegacy* column;
    uint32_t row;

    // Object slices this Arg to become a CompactArg.
    CompactArg ToCompactArg() const { return CompactArg(*this); }
  };
  static_assert(std::is_trivially_destructible<Arg>::value,
                "Args must be trivially destructible");

  struct ArgHasher {
    uint64_t operator()(const Arg& arg) const noexcept {
      base::Hasher hash;
      hash.Update(arg.key.raw_id());
      // We don't hash arg.flat_key because it's a subsequence of arg.key.
      switch (arg.value.type) {
        case Variadic::Type::kInt:
          hash.Update(arg.value.int_value);
          break;
        case Variadic::Type::kUint:
          hash.Update(arg.value.uint_value);
          break;
        case Variadic::Type::kString:
          hash.Update(arg.value.string_value.raw_id());
          break;
        case Variadic::Type::kReal:
          hash.Update(arg.value.real_value);
          break;
        case Variadic::Type::kPointer:
          hash.Update(arg.value.pointer_value);
          break;
        case Variadic::Type::kBool:
          hash.Update(arg.value.bool_value);
          break;
        case Variadic::Type::kJson:
          hash.Update(arg.value.json_value.raw_id());
          break;
        case Variadic::Type::kNull:
          hash.Update(0);
          break;
      }
      return hash.digest();
    }
  };

  explicit GlobalArgsTracker(TraceStorage* storage);

  // Assumes that the interval [begin, end) of |args| has args with the same key
  // grouped together.
  ArgSetId AddArgSet(const Arg* args, uint32_t begin, uint32_t end) {
    base::SmallVector<uint32_t, 64> valid_indexes;

    // TODO(eseckler): Also detect "invalid" key combinations in args sets (e.g.
    // "foo" and "foo.bar" in the same arg set)?
    for (uint32_t i = begin; i < end; i++) {
      if (!valid_indexes.empty() &&
          args[valid_indexes.back()].key == args[i].key) {
        // Last arg had the same key as this one. In case of kSkipIfExists, skip
        // this arg. In case of kAddOrUpdate, remove the last arg and add this
        // arg instead.
        if (args[i].update_policy == UpdatePolicy::kSkipIfExists) {
          continue;
        } else {
          PERFETTO_DCHECK(args[i].update_policy == UpdatePolicy::kAddOrUpdate);
          valid_indexes.pop_back();
        }
      }

      valid_indexes.emplace_back(i);
    }

    base::Hasher hash;
    for (uint32_t i : valid_indexes) {
      hash.Update(ArgHasher()(args[i]));
    }

    auto& arg_table = *storage_->mutable_arg_table();

    ArgSetHash digest = hash.digest();
    auto it_and_inserted =
        arg_row_for_hash_.Insert(digest, arg_table.row_count());
    if (!it_and_inserted.second) {
      // Already inserted.
      return arg_table[*it_and_inserted.first].arg_set_id();
    }

    // Taking size() after the Insert() ensures that nothing has an id == 0
    // (0 == kInvalidArgSetId).
    auto id = static_cast<uint32_t>(arg_row_for_hash_.size());
    for (uint32_t i : valid_indexes) {
      const auto& arg = args[i];

      tables::ArgTable::Row row;
      row.arg_set_id = id;
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
    return id;
  }

  // Exposed for making tests easier to write.
  ArgSetId AddArgSet(const std::vector<Arg>& args,
                     uint32_t begin,
                     uint32_t end) {
    return AddArgSet(args.data(), begin, end);
  }

 private:
  using ArgSetHash = uint64_t;

  base::FlatHashMap<ArgSetHash, uint32_t, base::AlreadyHashed<ArgSetHash>>
      arg_row_for_hash_;

  TraceStorage* storage_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_GLOBAL_ARGS_TRACKER_H_
