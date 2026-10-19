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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_VALUE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_VALUE_H_

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::core::exec {

// A value a run of a plan compares a column with, already cast to the column's
// storage type for one comparison, exactly as the dataframe interpreter casts
// it (see filter_value_cast.h): a value, or that every or no row matches.
//
// Whoever runs a plan keeps one per condition from run to run, and casts each
// run's values into them with casters it picked when the plan was built (see
// GetFilterValueCaster). Everything after only reads them: nothing is
// allocated, and nothing dispatches on the column's type or the comparison,
// which matters when each run looks up one row.
struct FilterValue {
  filter::CastFilterValueResult value =
      filter::CastFilterValueResult::NoneMatch();
  // For a string compared by = or !=: the string's id in the pool.
  StringPool::Id string_id = StringPool::Id::Null();
  // For a string compared by order: its text, which `value` points to. Its
  // buffer is reused, so only a string longer than any before allocates.
  std::string text;
};

// Casts a value for one storage type and comparison.
template <typename Fetcher>
using FilterValueCaster =
    void (*)(Fetcher&, uint32_t, const StringPool&, FilterValue*);

namespace internal {

// Casts the value `fetcher` has at `index` into `out`, for a column of
// storage type T compared by O. `pool` holds the column's strings.
template <typename T, typename O, typename Fetcher>
PERFETTO_ALWAYS_INLINE void CastFilterValueFor(Fetcher& fetcher,
                                               uint32_t index,
                                               const StringPool& pool,
                                               FilterValue* out) {
  out->value = filter::CastFilterValue<T>(index, fetcher, NonNullOp(O{}));
  if constexpr (std::is_same_v<T, String>) {
    if (out->value.validity != filter::CastFilterValueResult::kValid) {
      return;
    }
    const char* text = base::unchecked_get<const char*>(out->value.value);
    if constexpr (std::is_same_v<O, Eq> || std::is_same_v<O, Ne>) {
      // A string no row holds equals none of them, and differs from all.
      std::optional<StringPool::Id> id = pool.GetId(base::StringView(text));
      if (!id) {
        out->value = std::is_same_v<O, Eq>
                         ? filter::CastFilterValueResult::NoneMatch()
                         : filter::CastFilterValueResult::AllMatch();
        return;
      }
      out->string_id = *id;
      // Whatever `text` points to need not outlive the cast.
      out->value.value = static_cast<const char*>(nullptr);
    } else {
      out->text.assign(text);
      out->value.value = out->text.c_str();
    }
  } else {
    base::ignore_result(pool);
  }
}

template <typename T, typename Fetcher>
FilterValueCaster<Fetcher> FilterValueCasterFor(const Op& op) {
  switch (op.index()) {
    case Op::GetTypeIndex<Eq>():
      return &CastFilterValueFor<T, Eq, Fetcher>;
    case Op::GetTypeIndex<Ne>():
      return &CastFilterValueFor<T, Ne, Fetcher>;
    case Op::GetTypeIndex<Lt>():
      return &CastFilterValueFor<T, Lt, Fetcher>;
    case Op::GetTypeIndex<Le>():
      return &CastFilterValueFor<T, Le, Fetcher>;
    case Op::GetTypeIndex<Gt>():
      return &CastFilterValueFor<T, Gt, Fetcher>;
    case Op::GetTypeIndex<Ge>():
      return &CastFilterValueFor<T, Ge, Fetcher>;
    default:
      PERFETTO_FATAL("Unsupported filter operator");
  }
}

}  // namespace internal

// The caster for a column of storage type `type` compared by `op`, one of =,
// !=, <, <=, >, >=. Picked once, when a plan is built.
template <typename Fetcher>
FilterValueCaster<Fetcher> GetFilterValueCaster(StorageType type,
                                                const Op& op) {
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
      return internal::FilterValueCasterFor<Id, Fetcher>(op);
    case StorageType::GetTypeIndex<Uint32>():
      return internal::FilterValueCasterFor<Uint32, Fetcher>(op);
    case StorageType::GetTypeIndex<Int32>():
      return internal::FilterValueCasterFor<Int32, Fetcher>(op);
    case StorageType::GetTypeIndex<Int64>():
      return internal::FilterValueCasterFor<Int64, Fetcher>(op);
    case StorageType::GetTypeIndex<Double>():
      return internal::FilterValueCasterFor<Double, Fetcher>(op);
    case StorageType::GetTypeIndex<String>():
      return internal::FilterValueCasterFor<String, Fetcher>(op);
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_VALUE_H_
