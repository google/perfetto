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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_PARAM_VALUE_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_PARAM_VALUE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/regex.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/variant.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/filter_value_cast.h"
#include "src/trace_processor/core/common/op_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/util/glob.h"

namespace perfetto::trace_processor::core::exec {

// A GLOB literal's pattern, compiled, and the strings matched with it so far:
// a string's result depends only on the pattern, so whatever reads the literal
// fills this in as it goes.
struct GlobMatch {
  explicit GlobMatch(base::StringView pattern)
      : matcher(util::GlobMatcher::FromPattern(pattern)) {}

  util::GlobMatcher matcher;
  // Rows matched one at a time.
  uint64_t rows = 0;
  // The result of every string with an id below `matched_below`, once the
  // pool has been matched. Strings interned after are above it.
  BitVector matches;
  uint32_t matched_below = 0;
};

// A param of a run: a value its operators compare a column with, already cast
// to the column's storage type for one comparison (see filter_value_cast.h): a
// value, or that every or no row matches.
//
// Whoever runs a plan keeps one per param from run to run, and casts each
// run's values into them with casters it picked when the plan was built (see
// GetParamCaster). Everything after only reads them, except a GLOB literal's
// GlobMatch, which is filled in as it is read. Nothing on a run's path
// allocates, and nothing dispatches on the column's type or the comparison,
// which matters when each run looks up one row.
struct ParamValue {
  // What only string ordering, REGEXP, GLOB and IN need, allocated on first
  // use so that the params of every other comparison stay small.
  struct Extra {
    // For a string compared by order: its text, which `value` points to. Its
    // buffer is reused, so only a string longer than any before allocates.
    std::string text;
    // For REGEXP: the pattern, compiled once a run rather than once a batch.
    std::optional<base::Regex> regex;
    // For IN: the list's values, while `validity` says whether any can match.
    // Its allocations are reused from run to run.
    filter::CastFilterValueListResult::Ptr list;
    // For a GLOB literal with wildcards (see AttachGlob).
    std::unique_ptr<GlobMatch> glob;
  };

  Extra& EnsureExtra() {
    if (!extra) {
      extra = std::make_unique<Extra>();
    }
    return *extra;
  }

  const std::string& text() const { return extra->text; }
  const base::Regex& regex() const { return *extra->regex; }
  const filter::CastFilterValueListResult& list() const { return *extra->list; }
  GlobMatch* glob() const { return extra ? extra->glob.get() : nullptr; }

  // The value, read only while `validity` is kValid.
  filter::CastFilterValueResult::Value value =
      filter::CastFilterValueResult::Id{0};
  filter::CastFilterValueResult::Validity validity =
      filter::CastFilterValueResult::kNoneMatch;
  // For a string compared by = or !=: the string's id in the pool.
  StringPool::Id string_id = StringPool::Id::Null();
  std::unique_ptr<Extra> extra;
};
static_assert(sizeof(ParamValue) <= 32);

// Gives `value`, a GLOB literal just cast, its GlobMatch, so that it is
// matched against the pool once however many runs read it. A pattern without
// wildcards still compares string ids, and needs none.
inline void AttachGlob(ParamValue* value) {
  if (value->extra) {
    value->extra->glob.reset();
  }
  if (value->validity != filter::CastFilterValueResult::kValid) {
    return;
  }
  auto glob = std::make_unique<GlobMatch>(base::StringView(value->text()));
  if (!glob->matcher.IsEquality()) {
    value->EnsureExtra().glob = std::move(glob);
  }
}

namespace internal {

// Casts the value `fetcher` has at `index` into `out`, for a numeric column
// of storage type T compared by `op`.
template <typename T, typename Fetcher>
void CastParamFor(Fetcher& fetcher,
                  uint32_t index,
                  NonNullOp op,
                  const StringPool&,
                  ParamValue* out) {
  filter::CastFilterValueResult res =
      filter::CastFilterValue<T>(index, fetcher, op);
  out->validity = res.validity;
  if (PERFETTO_LIKELY(res.validity == filter::CastFilterValueResult::kValid)) {
    out->value = res.value;
  }
}

// Casts a string compared by = (kEq) or != to its id in `pool`, leaving
// `value` unset. Apart from the casts for ordering, GLOB and REGEXP, which
// would give every lookup their stack frame.
template <bool kEq, typename Fetcher>
void CastStringEqParam(Fetcher& fetcher,
                       uint32_t index,
                       NonNullOp,
                       const StringPool& pool,
                       ParamValue* out) {
  auto type = fetcher.GetValueType(index);
  if (PERFETTO_LIKELY(type == Fetcher::kString)) {
    const char* text = fetcher.GetStringValue(index);
    std::optional<StringPool::Id> id = pool.GetId(base::StringView(text));
    if (PERFETTO_LIKELY(id.has_value())) {
      out->validity = filter::CastFilterValueResult::kValid;
      out->string_id = *id;
    } else {
      // A string no row holds equals none of them, and differs from all.
      out->validity = kEq ? filter::CastFilterValueResult::kNoneMatch
                          : filter::CastFilterValueResult::kAllMatch;
    }
    return;
  }
  // As CastFilterValue<String> does: null matches nothing, and a number
  // equals no string and differs from all.
  PERFETTO_DCHECK(type == Fetcher::kNull || type == Fetcher::kInt64 ||
                  type == Fetcher::kDouble);
  out->validity = type == Fetcher::kNull || kEq
                      ? filter::CastFilterValueResult::kNoneMatch
                      : filter::CastFilterValueResult::kAllMatch;
}

// Casts a string compared by order, GLOB or REGEXP into `out`.
template <typename Fetcher>
void CastStringOtherParam(Fetcher& fetcher,
                          uint32_t index,
                          NonNullOp op,
                          const StringPool&,
                          ParamValue* out) {
  filter::CastFilterValueResult res =
      filter::CastFilterValue<String>(index, fetcher, op);
  out->validity = res.validity;
  if (res.validity != filter::CastFilterValueResult::kValid) {
    return;
  }
  ParamValue::Extra& extra = out->EnsureExtra();
  extra.text.assign(base::unchecked_get<const char*>(res.value));
  out->value = extra.text.c_str();
  if (op.Is<Regex>()) {
    // As the interpreter does, a pattern which does not compile matches
    // nothing.
    base::StatusOr<base::Regex> regex = base::Regex::Create(extra.text);
    if (!regex.ok()) {
      out->validity = filter::CastFilterValueResult::kNoneMatch;
      return;
    }
    extra.regex = std::move(*regex);
  }
}

// Casts the IN list `fetcher` iterates at `index` into `out`, for a column of
// storage type T.
template <typename T, typename Fetcher>
void CastParamListFor(Fetcher& fetcher,
                      uint32_t index,
                      NonNullOp op,
                      const StringPool& pool,
                      ParamValue* out) {
  auto& list = out->EnsureExtra().list;
  if (!list) {
    list = std::make_unique<filter::CastFilterValueListResult>();
    list->Init<T>();
  } else {
    list->Clear<T>();
  }
  filter::CastFilterValueList<T>(index, fetcher, op, &pool, *list);
  out->validity = list->validity;
}

}  // namespace internal

// Casts a value for one storage type and comparison, picked once, when a plan
// is built (see GetParamCaster).
template <typename Fetcher>
class ParamCaster {
 public:
  using Fn =
      void (*)(Fetcher&, uint32_t, NonNullOp, const StringPool&, ParamValue*);

  ParamCaster(Fn fn, NonNullOp op) : fn_(fn), op_(op) {}

  void operator()(Fetcher& fetcher,
                  uint32_t index,
                  const StringPool& pool,
                  ParamValue* out) const {
    fn_(fetcher, index, op_, pool, out);
  }

 private:
  Fn fn_;
  NonNullOp op_;
};

// The caster for a column of storage type `type` compared by `op`, one of =,
// !=, <, <=, >, >= or, for a string column, GLOB or REGEXP.
template <typename Fetcher>
ParamCaster<Fetcher> GetParamCaster(StorageType type, const Op& op) {
  PERFETTO_CHECK(op.IsAnyOf<RangeOp>() || op.Is<Eq>() || op.Is<Ne>() ||
                 (op.IsAnyOf<OnlyStringOp>() && type.Is<String>()));
  NonNullOp non_null = *op.TryDowncast<NonNullOp>();
  switch (type.index()) {
    case StorageType::GetTypeIndex<Uint32>():
      return {&internal::CastParamFor<Uint32, Fetcher>, non_null};
    case StorageType::GetTypeIndex<Int32>():
      return {&internal::CastParamFor<Int32, Fetcher>, non_null};
    case StorageType::GetTypeIndex<Int64>():
      return {&internal::CastParamFor<Int64, Fetcher>, non_null};
    case StorageType::GetTypeIndex<Double>():
      return {&internal::CastParamFor<Double, Fetcher>, non_null};
    case StorageType::GetTypeIndex<String>():
      if (non_null.Is<Eq>()) {
        return {&internal::CastStringEqParam<true, Fetcher>, non_null};
      }
      if (non_null.Is<Ne>()) {
        return {&internal::CastStringEqParam<false, Fetcher>, non_null};
      }
      return {&internal::CastStringOtherParam<Fetcher>, non_null};
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

// The caster of IN lists for a column of storage type `type`. `Fetcher` must
// iterate lists (see ValueFetcher::IteratorInit).
template <typename Fetcher>
ParamCaster<Fetcher> GetParamListCaster(StorageType type) {
  NonNullOp eq = Eq{};
  switch (type.index()) {
    case StorageType::GetTypeIndex<Uint32>():
      return {&internal::CastParamListFor<Uint32, Fetcher>, eq};
    case StorageType::GetTypeIndex<Int32>():
      return {&internal::CastParamListFor<Int32, Fetcher>, eq};
    case StorageType::GetTypeIndex<Int64>():
      return {&internal::CastParamListFor<Int64, Fetcher>, eq};
    case StorageType::GetTypeIndex<Double>():
      return {&internal::CastParamListFor<Double, Fetcher>, eq};
    case StorageType::GetTypeIndex<String>():
      return {&internal::CastParamListFor<String, Fetcher>, eq};
    default:
      PERFETTO_FATAL("Unsupported storage type");
  }
}

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_PARAM_VALUE_H_
