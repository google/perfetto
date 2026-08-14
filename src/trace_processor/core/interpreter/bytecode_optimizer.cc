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

#include "src/trace_processor/core/interpreter/bytecode_optimizer.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::interpreter {
namespace {

template <typename T>
const T& As(const Bytecode& bytecode) {
  return static_cast<const T&>(bytecode);
}

bool Is(const Bytecode& bytecode, uint32_t option) {
  return bytecode.option == option;
}

bool HasCommonIdPrefix(const BytecodeVector& bytecode) {
  if (bytecode.size() < 3 || !Is(bytecode[0], Index<InitRange>()) ||
      !Is(bytecode[1], Index<CastFilterValue<Id>>()) ||
      !Is(bytecode[2], Index<SortedFilter<Id, EqualRange>>())) {
    return false;
  }
  const auto& init = As<InitRange>(bytecode[0]);
  const auto& cast = As<CastFilterValue<Id>>(bytecode[1]);
  const auto& filter = As<SortedFilter<Id, EqualRange>>(bytecode[2]);
  return init.arg<InitRange::dest_register>().index ==
             filter.arg<SortedFilterBase::update_register>().index &&
         cast.arg<CastFilterValueBase::write_register>().index ==
             filter.arg<SortedFilterBase::val_register>().index &&
         cast.arg<CastFilterValueBase::op>().Is<Eq>() &&
         filter.arg<SortedFilterBase::write_result_to>().Is<BothBounds>();
}

std::optional<uint32_t> GetFirstPredicateType(const Bytecode& cast,
                                              const Bytecode& filter) {
#define PERFETTO_MATCH_TYPE(type)                  \
  if (Is(cast, Index<CastFilterValue<type>>()) &&  \
      Is(filter, Index<LinearFilterEq<type>>())) { \
    return NonIdStorageType::GetTypeIndex<type>(); \
  }
  PERFETTO_MATCH_TYPE(Uint32)
  PERFETTO_MATCH_TYPE(Int32)
  PERFETTO_MATCH_TYPE(Int64)
  PERFETTO_MATCH_TYPE(Double)
  PERFETTO_MATCH_TYPE(String)
#undef PERFETTO_MATCH_TYPE
  return std::nullopt;
}

std::optional<uint32_t> GetSubsequentPredicateType(const Bytecode& cast,
                                                   const Bytecode& filter) {
#define PERFETTO_MATCH_NUMERIC_TYPE(type)               \
  if (Is(cast, Index<CastFilterValue<type>>()) &&       \
      Is(filter, Index<NonStringFilter<type, Eq>>())) { \
    return NonIdStorageType::GetTypeIndex<type>();      \
  }
  PERFETTO_MATCH_NUMERIC_TYPE(Uint32)
  PERFETTO_MATCH_NUMERIC_TYPE(Int32)
  PERFETTO_MATCH_NUMERIC_TYPE(Int64)
  PERFETTO_MATCH_NUMERIC_TYPE(Double)
#undef PERFETTO_MATCH_NUMERIC_TYPE
  if (Is(cast, Index<CastFilterValue<String>>()) &&
      Is(filter, Index<StringFilter<Eq>>())) {
    return NonIdStorageType::GetTypeIndex<String>();
  }
  return std::nullopt;
}

void AppendPredicate(BytecodeVector& optimized,
                     FilterValueHandle fval_handle,
                     ReadHandle<StoragePtr> storage_register,
                     uint32_t storage_type) {
  optimized.emplace_back();
  auto& predicate = static_cast<SingleRowEqPredicate&>(optimized.back());
  predicate.option = Index<SingleRowEqPredicate>();
  predicate.arg<SingleRowEqPredicate::fval_handle>() = fval_handle;
  predicate.arg<SingleRowEqPredicate::storage_register>() = storage_register;
  predicate.arg<SingleRowEqPredicate::storage_type>() = storage_type;
}

SingleRowQuery MakeSingleRowQuery(const BytecodeVector& bytecode,
                                  uint32_t predicate_count) {
  const auto& init = As<InitRange>(bytecode[0]);
  const auto& id_cast = As<CastFilterValue<Id>>(bytecode[1]);
  SingleRowQuery query;
  query.option = Index<SingleRowQuery>();
  query.arg<SingleRowQuery::row_count>() = init.arg<InitRange::size>();
  query.arg<SingleRowQuery::id_fval_handle>() =
      id_cast.arg<CastFilterValueBase::fval_handle>();
  query.arg<SingleRowQuery::predicate_count>() = predicate_count;
  return query;
}

bool TryFuseIdOnly(BytecodeVector& bytecode) {
  if (bytecode.size() != 5 || !HasCommonIdPrefix(bytecode) ||
      !Is(bytecode[3], Index<AllocateIndices>()) ||
      !Is(bytecode[4], Index<Iota>())) {
    return false;
  }
  const auto& init = As<InitRange>(bytecode[0]);
  const auto& alloc = As<AllocateIndices>(bytecode[3]);
  const auto& iota = As<Iota>(bytecode[4]);
  if (alloc.arg<AllocateIndices::size>() > 1 ||
      init.arg<InitRange::dest_register>().index !=
          iota.arg<Iota::source_register>().index ||
      alloc.arg<AllocateIndices::dest_span_register>().index !=
          iota.arg<Iota::update_register>().index) {
    return false;
  }

  BytecodeVector optimized;
  optimized.emplace_back(MakeSingleRowQuery(bytecode, 0));
  bytecode = std::move(optimized);
  return true;
}

bool TryFuseIdAndColumnEq(BytecodeVector& bytecode) {
  if (bytecode.size() < 6 || !HasCommonIdPrefix(bytecode) ||
      !Is(bytecode[4], Index<AllocateIndices>())) {
    return false;
  }
  std::optional<uint32_t> storage_type =
      GetFirstPredicateType(bytecode[3], bytecode[5]);
  if (!storage_type) {
    return false;
  }

  const auto& init = As<InitRange>(bytecode[0]);
  const auto& value_cast = As<CastFilterValueBase>(bytecode[3]);
  const auto& alloc = As<AllocateIndices>(bytecode[4]);
  const auto& filter = As<LinearFilterEqBase>(bytecode[5]);
  if (alloc.arg<AllocateIndices::size>() > 1 ||
      !value_cast.arg<CastFilterValueBase::op>().Is<Eq>() ||
      init.arg<InitRange::dest_register>().index !=
          filter.arg<LinearFilterEqBase::source_register>().index ||
      value_cast.arg<CastFilterValueBase::write_register>().index !=
          filter.arg<LinearFilterEqBase::filter_value_reg>().index ||
      alloc.arg<AllocateIndices::dest_span_register>().index !=
          filter.arg<LinearFilterEqBase::update_register>().index) {
    return false;
  }

  auto output_register = alloc.arg<AllocateIndices::dest_span_register>();
  BytecodeVector optimized;
  optimized.emplace_back(MakeSingleRowQuery(bytecode, 0));
  AppendPredicate(optimized, value_cast.arg<CastFilterValueBase::fval_handle>(),
                  filter.arg<LinearFilterEqBase::storage_register>(),
                  *storage_type);
  for (size_t i = 6; i < bytecode.size(); i += 2) {
    if (i + 1 >= bytecode.size()) {
      return false;
    }
    storage_type = GetSubsequentPredicateType(bytecode[i], bytecode[i + 1]);
    if (!storage_type) {
      return false;
    }
    const auto& cast = As<CastFilterValueBase>(bytecode[i]);
    if (!cast.arg<CastFilterValueBase::op>().Is<Eq>()) {
      return false;
    }

    ReadHandle<StoragePtr> predicate_storage;
    ReadHandle<CastFilterValueResult> predicate_value;
    ReadHandle<Span<uint32_t>> predicate_source;
    RwHandle<Span<uint32_t>> predicate_update;
    if (*storage_type == NonIdStorageType::GetTypeIndex<String>()) {
      const auto& predicate_filter = As<StringFilterBase>(bytecode[i + 1]);
      predicate_storage =
          predicate_filter.arg<StringFilterBase::storage_register>();
      predicate_value = predicate_filter.arg<StringFilterBase::val_register>();
      predicate_source =
          predicate_filter.arg<StringFilterBase::source_register>();
      predicate_update =
          predicate_filter.arg<StringFilterBase::update_register>();
    } else {
      const auto& predicate_filter = As<NonStringFilterBase>(bytecode[i + 1]);
      predicate_storage =
          predicate_filter.arg<NonStringFilterBase::storage_register>();
      predicate_value =
          predicate_filter.arg<NonStringFilterBase::val_register>();
      predicate_source =
          predicate_filter.arg<NonStringFilterBase::source_register>();
      predicate_update =
          predicate_filter.arg<NonStringFilterBase::update_register>();
    }
    if (cast.arg<CastFilterValueBase::write_register>().index !=
            predicate_value.index ||
        output_register.index != predicate_source.index ||
        output_register.index != predicate_update.index) {
      return false;
    }
    AppendPredicate(optimized, cast.arg<CastFilterValueBase::fval_handle>(),
                    predicate_storage, *storage_type);
  }

  auto& query = static_cast<SingleRowQuery&>(optimized.front());
  query.arg<SingleRowQuery::predicate_count>() =
      static_cast<uint32_t>(optimized.size() - 1);
  bytecode = std::move(optimized);
  return true;
}

}  // namespace

void OptimizeBytecode(BytecodeVector& bytecode) {
  if (TryFuseIdAndColumnEq(bytecode)) {
    return;
  }
  TryFuseIdOnly(bytecode);
}

}  // namespace perfetto::trace_processor::core::interpreter
