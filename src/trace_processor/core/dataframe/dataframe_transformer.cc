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
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/range.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::dataframe {

namespace {
namespace i = interpreter;

// Scratch slot used for filter bytecode within DataframeTransformer.
// Starts at 20 to avoid collision with TreeTransformer::ScratchSlot (0-9).
constexpr uint32_t kDtFilterBytecodeSlot = 20;

}  // namespace

DataframeTransformer::DataframeTransformer(i::BytecodeBuilder& builder,
                                           const Dataframe& df)
    : builder_(builder), cache_(&builder), max_row_count_(df.row_count()) {
  columns_ = df.columns_;
  column_names_ = df.column_names();
  indexes_ = df.indexes_;
}

base::StatusOr<i::RwHandle<BitVector>> DataframeTransformer::Filter(
    std::vector<FilterSpec>& specs) {
  // Build input indices.
  auto range_reg = builder_.AllocateRegister<Range>();
  {
    using B = i::InitRange;
    auto& op = builder_.AddOpcode<B>(i::Index<B>());
    op.arg<B::size>() = max_row_count_;
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
    auto filter_scratch =
        builder_.AllocateScratch(kDtFilterBytecodeSlot, max_row_count_);
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
    builder_.ReleaseScratch(kDtFilterBytecodeSlot);
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

std::optional<uint32_t> DataframeTransformer::FindColumn(
    const std::string& name) const {
  for (uint32_t i = 0; i < column_names_.size(); ++i) {
    if (column_names_[i] == name) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace perfetto::trace_processor::core::dataframe
