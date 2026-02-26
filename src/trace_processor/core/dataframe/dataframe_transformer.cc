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

#include "src/trace_processor/core/dataframe/dataframe_transformer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/query_plan.h"
#include "src/trace_processor/core/dataframe/register_cache.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/interpreter/bytecode_builder.h"
#include "src/trace_processor/core/interpreter/bytecode_instructions.h"
#include "src/trace_processor/core/interpreter/bytecode_registers.h"
#include "src/trace_processor/core/interpreter/interpreter_types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/slab.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::dataframe {

namespace {
namespace i = interpreter;

}  // namespace

DataframeTransformer::DataframeTransformer(i::BytecodeBuilder& builder,
                                           const Dataframe& df)
    : builder_(builder), cache_(&builder), max_row_count_(df.row_count()) {
  columns_ = df.columns_;
  column_names_ = df.column_names();
  indexes_ = df.indexes_;
}

// static
std::shared_ptr<Column> DataframeTransformer::MakeColumn(
    StorageType type,
    Nullability nullability) {
  NullStorage null_storage(NullStorage::NonNull{});
  if (!nullability.Is<NonNull>()) {
    null_storage =
        NullStorage(NullStorage::DenseNull{BitVector::CreateWithSize(0)});
  }
  if (type.Is<Int32>()) {
    return std::make_shared<Column>(Column{Storage(Storage::Int32{}),
                                           std::move(null_storage), Unsorted{},
                                           DuplicateState(HasDuplicates{})});
  }
  if (type.Is<Int64>()) {
    return std::make_shared<Column>(Column{Storage(Storage::Int64{}),
                                           std::move(null_storage), Unsorted{},
                                           DuplicateState(HasDuplicates{})});
  }
  if (type.Is<Double>()) {
    return std::make_shared<Column>(Column{Storage(Storage::Double{}),
                                           std::move(null_storage), Unsorted{},
                                           DuplicateState(HasDuplicates{})});
  }
  if (type.Is<String>()) {
    return std::make_shared<Column>(Column{Storage(Storage::String{}),
                                           std::move(null_storage), Unsorted{},
                                           DuplicateState(HasDuplicates{})});
  }
  return std::make_shared<Column>(Column{Storage(Storage::Uint32{}),
                                         std::move(null_storage), Unsorted{},
                                         DuplicateState(HasDuplicates{})});
}

base::StatusOr<i::RwHandle<BitVector>> DataframeTransformer::Filter(
    std::vector<FilterSpec>& specs) {
  // Build input indices.
  auto range_reg = builder_.AllocateRegister<Range>();
  if (!gathered_) {
    using B = i::InitRange;
    auto& op = builder_.AddOpcode<B>(i::Index<B>());
    op.arg<B::size>() = max_row_count_;
    op.arg<B::dest_register>() = range_reg;
  } else {
    using B = i::InitRangeFromSpan;
    auto& op = builder_.AddOpcode<B>(i::Index<B>());
    op.arg<B::source_span_register>() = row_count_span_;
    op.arg<B::dest_register>() = range_reg;
  }

  // Call Filter with the register cache.
  ASSIGN_OR_RETURN(auto filter_result,
                   QueryPlanBuilder::Filter(
                       builder_, i::RwHandle<Range>(range_reg), max_row_count_,
                       columns_, indexes_, cache_, specs));

  // Capture register init specs from newly allocated registers.
  for (const auto& init : filter_result.register_inits) {
    register_inits_.emplace_back(init);
  }

  // Convert filtered result to bitvector.
  auto bv_reg = builder_.AllocateRegister<BitVector>();
  if (auto* range_ptr =
          std::get_if<i::RwHandle<Range>>(&filter_result.indices)) {
    auto filter_scratch = builder_.AllocateScratch(max_row_count_);
    {
      using Iota = i::Iota;
      auto& op = builder_.AddOpcode<Iota>(i::Index<Iota>());
      op.arg<Iota::source_register>() = *range_ptr;
      op.arg<Iota::update_register>() = filter_scratch.span;
    }
    {
      using SpanToBv = i::IndexSpanToBitvector;
      auto& op = builder_.AddOpcode<SpanToBv>(i::Index<SpanToBv>());
      op.arg<SpanToBv::indices_register>() = filter_scratch.span;
      op.arg<SpanToBv::bitvector_size>() = max_row_count_;
      op.arg<SpanToBv::dest_register>() = bv_reg;
    }
    builder_.ReleaseScratch(filter_scratch);
  } else {
    auto span = std::get<i::RwHandle<Span<uint32_t>>>(filter_result.indices);
    auto& op = builder_.AddOpcode<i::IndexSpanToBitvector>(
        i::Index<i::IndexSpanToBitvector>());
    op.arg<i::IndexSpanToBitvector::indices_register>() = span;
    op.arg<i::IndexSpanToBitvector::bitvector_size>() = max_row_count_;
    op.arg<i::IndexSpanToBitvector::dest_register>() = bv_reg;
  }

  return bv_reg;
}

i::ReadHandle<i::StoragePtr> DataframeTransformer::StorageRegisterFor(
    uint32_t col_idx) {
  auto [reg, inserted] =
      cache_.GetOrAllocate<i::StoragePtr>(columns_[col_idx].get(), kStorageReg);
  if (inserted) {
    StorageType type = columns_[col_idx]->storage.type();
    register_inits_.emplace_back(RegisterInit{reg.index,
                                              type.Upcast<RegisterInit::Type>(),
                                              static_cast<uint16_t>(col_idx)});
  }
  return reg;
}

StorageType DataframeTransformer::GetStorageType(uint32_t col_idx) const {
  return columns_[col_idx]->storage.type();
}

Nullability DataframeTransformer::GetNullability(uint32_t col_idx) const {
  return columns_[col_idx]->null_storage.nullability();
}

i::ReadHandle<const BitVector*> DataframeTransformer::NullBitvectorRegisterFor(
    uint32_t col_idx) {
  auto [reg, inserted] = cache_.GetOrAllocate<const BitVector*>(
      columns_[col_idx].get(), kNullBvReg);
  if (inserted) {
    register_inits_.emplace_back(RegisterInit{reg.index,
                                              RegisterInit::NullBitvector{},
                                              static_cast<uint16_t>(col_idx)});
  }
  return reg;
}

std::optional<uint32_t> DataframeTransformer::FindColumn(
    const std::string& name) const {
  for (uint32_t i = 0; i < column_names_.size(); ++i) {
    if (column_names_[i] == name) {
      return i;
    }
  }
  return std::nullopt;
}

uint32_t DataframeTransformer::AddColumn(
    const std::string& name,
    StorageType type,
    Nullability nullability,
    i::RwHandle<i::StoragePtr> storage_reg) {
  auto col_idx = static_cast<uint32_t>(columns_.size());
  auto col = MakeColumn(type, nullability);
  cache_.Set(col.get(), kStorageReg, i::HandleBase{storage_reg.index});
  columns_.push_back(std::move(col));
  column_names_.push_back(name);
  return col_idx;
}

void DataframeTransformer::GatherAllColumns(
    i::ReadHandle<Span<uint32_t>> indices) {
  row_count_span_ = indices;

  gather_state_.resize(columns_.size());
  for (uint32_t col = 0; col < columns_.size(); ++col) {
    StorageType type = columns_[col]->storage.type();

    if (type.Is<Id>()) {
      continue;
    }

    auto non_id = type.TryDowncast<i::NonIdStorageType>();
    if (!non_id) {
      continue;
    }

    // Allocate gather state registers.
    auto slab_reg = builder_.AllocateRegister<Slab<uint8_t>>();
    auto null_bv_reg = builder_.AllocateRegister<BitVector>();
    auto null_bv_ptr_reg = builder_.AllocateRegister<const BitVector*>();
    auto dest_storage_reg = builder_.AllocateRegister<i::StoragePtr>();

    // Get source storage register (allocate + RegisterInit if needed).
    auto source_storage_handle = StorageRegisterFor(col);

    // Get null bitvector register (allocate + RegisterInit if needed).
    auto [null_reg, null_inserted] =
        cache_.GetOrAllocate<const BitVector*>(columns_[col].get(), kNullBvReg);
    if (null_inserted) {
      register_inits_.emplace_back(RegisterInit{null_reg.index,
                                                RegisterInit::NullBitvector{},
                                                static_cast<uint16_t>(col)});
    }

    Nullability nullability = columns_[col]->null_storage.nullability();

    // Emit GatherColumn bytecode.
    using GC = i::GatherColumnBase;
    auto& op = builder_.AddOpcode<GC>(i::Index<i::GatherColumn>(*non_id));
    op.arg<GC::source_storage_register>() = source_storage_handle;
    op.arg<GC::indices_register>() = indices;
    op.arg<GC::source_null_bv_register>() = null_reg;
    op.arg<GC::source_nullability>() = nullability.index();
    op.arg<GC::dest_slab_register>() = slab_reg;
    op.arg<GC::dest_null_bv_register>() = null_bv_reg;
    op.arg<GC::dest_null_bv_ptr_register>() = null_bv_ptr_reg;
    op.arg<GC::dest_storage_register>() = dest_storage_reg;

    gather_state_[col] =
        GatherState{slab_reg, null_bv_reg, null_bv_ptr_reg, dest_storage_reg};

    // Replace column with new object (new pointer = natural cache miss).
    columns_[col] = MakeColumn(type, nullability);

    // Pre-populate cache with new Column* -> new gathered registers.
    cache_.Set(columns_[col].get(), kStorageReg,
               i::HandleBase{dest_storage_reg.index});
    cache_.Set(columns_[col].get(), kNullBvReg,
               i::HandleBase{null_bv_ptr_reg.index});
  }

  gathered_ = true;
  indexes_.clear();
}

}  // namespace perfetto::trace_processor::core::dataframe
