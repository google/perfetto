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
#include <memory>
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
  // For IN: the list's values, while `value` says whether any can match. Its
  // allocations are reused from run to run.
  filter::CastFilterValueListResult::Ptr list;
};

namespace internal {

// Casts the value `fetcher` has at `index` into `out`, for a column of
// storage type T compared by `op`. `pool` holds the column's strings.
template <typename T, typename Fetcher>
void CastFilterValueFor(Fetcher& fetcher,
                        uint32_t index,
                        NonNullOp op,
                        const StringPool& pool,
                        FilterValue* out) {
  out->value = filter::CastFilterValue<T>(index, fetcher, op);
  if constexpr (std::is_same_v<T, String>) {
    if (out->value.validity != filter::CastFilterValueResult::kValid) {
      return;
    }
    const char* text = base::unchecked_get<const char*>(out->value.value);
    if (op.Is<Eq>() || op.Is<Ne>()) {
      // A string no row holds equals none of them, and differs from all.
      std::optional<StringPool::Id> id = pool.GetId(base::StringView(text));
      if (!id) {
        out->value = op.Is<Eq>() ? filter::CastFilterValueResult::NoneMatch()
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

// Casts the IN list `fetcher` iterates at `index` into `out`, for a column of
// storage type T.
template <typename T, typename Fetcher>
void CastFilterValueListFor(Fetcher& fetcher,
                            uint32_t index,
                            NonNullOp op,
                            const StringPool& pool,
                            FilterValue* out) {
  auto& list = out->list;
  if (!list) {
    list = std::make_unique<filter::CastFilterValueListResult>();
    list->Init<T>();
  } else {
    list->Clear<T>();
  }
  filter::CastFilterValueList<T>(index, fetcher, op, &pool, *list);
  out->value.validity = list->validity;
}

}  // namespace internal

// Casts a value for one storage type and comparison, picked once, when a plan
// is built (see GetFilterValueCaster).
template <typename Fetcher>
class FilterValueCaster {
 public:
  using Fn =
      void (*)(Fetcher&, uint32_t, NonNullOp, const StringPool&, FilterValue*);

  FilterValueCaster(Fn fn, NonNullOp op) : fn_(fn), op_(op) {}

  void operator()(Fetcher& fetcher,
                  uint32_t index,
                  const StringPool& pool,
                  FilterValue* out) const {
    fn_(fetcher, index, op_, pool, out);
  }

 private:
  Fn fn_;
  NonNullOp op_;
};

// The caster for a column of storage type `type` compared by `op`, one of =,
// !=, <, <=, >, >= or, for a string column, GLOB.
template <typename Fetcher>
FilterValueCaster<Fetcher> GetFilterValueCaster(StorageType type,
                                                const Op& op) {
  PERFETTO_CHECK(op.IsAnyOf<RangeOp>() || op.Is<Eq>() || op.Is<Ne>() ||
                 (op.Is<Glob>() && type.Is<String>()));
  NonNullOp non_null = *op.TryDowncast<NonNullOp>();
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
      return {&internal::CastFilterValueFor<Id, Fetcher>, non_null};
    case StorageType::GetTypeIndex<Uint32>():
      return {&internal::CastFilterValueFor<Uint32, Fetcher>, non_null};
    case StorageType::GetTypeIndex<Int32>():
      return {&internal::CastFilterValueFor<Int32, Fetcher>, non_null};
    case StorageType::GetTypeIndex<Int64>():
      return {&internal::CastFilterValueFor<Int64, Fetcher>, non_null};
    case StorageType::GetTypeIndex<Double>():
      return {&internal::CastFilterValueFor<Double, Fetcher>, non_null};
    case StorageType::GetTypeIndex<String>():
      return {&internal::CastFilterValueFor<String, Fetcher>, non_null};
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

// The caster of IN lists for a column of storage type `type`. `Fetcher` must
// iterate lists (see ValueFetcher::IteratorInit).
template <typename Fetcher>
FilterValueCaster<Fetcher> GetFilterValueListCaster(StorageType type) {
  NonNullOp eq = Eq{};
  switch (type.index()) {
    case StorageType::GetTypeIndex<Id>():
      return {&internal::CastFilterValueListFor<Id, Fetcher>, eq};
    case StorageType::GetTypeIndex<Uint32>():
      return {&internal::CastFilterValueListFor<Uint32, Fetcher>, eq};
    case StorageType::GetTypeIndex<Int32>():
      return {&internal::CastFilterValueListFor<Int32, Fetcher>, eq};
    case StorageType::GetTypeIndex<Int64>():
      return {&internal::CastFilterValueListFor<Int64, Fetcher>, eq};
    case StorageType::GetTypeIndex<Double>():
      return {&internal::CastFilterValueListFor<Double, Fetcher>, eq};
    case StorageType::GetTypeIndex<String>():
      return {&internal::CastFilterValueListFor<String, Fetcher>, eq};
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_FILTER_VALUE_H_
