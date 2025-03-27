/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_BYTECODE_INSTRUCTIONS_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_BYTECODE_INSTRUCTIONS_H_

#include <cstdint>
#include <string>
#include <variant>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode {

// Bytecode instructions - each represents a specific operation for query
// execution.

// Initializes a range register with a given size.
struct InitRange : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_2(uint32_t,
                                     size,
                                     reg::WriteHandle<Range>,
                                     dest_register);
};

// Allocates a slab of indices.
struct AllocateIndices : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_2(uint32_t,
                                     size,
                                     reg::WriteHandle<Slab<uint32_t>>,
                                     dest_register);
};

// Fills a memory region with sequential integers (0...n-1).
struct Iota : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_2(reg::ReadHandle<Range>,
                                     source_register,
                                     reg::RwHandle<Slab<uint32_t>>,
                                     update_register);
};

// Base class for casting filter value operations.
struct CastFilterValueBase : TemplatedBytecode1<Content> {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_3(FilterValueHandle,
                                     fval_handle,
                                     reg::WriteHandle<CastFilterValueResult>,
                                     write_register,
                                     NonNullOp,
                                     op);
};

// Specialized coercion for specific type T.
template <typename T>
struct CastFilterValue : CastFilterValueBase {
  static_assert(TS1::Contains<T>());
};

// Base for operations on sorted data.
struct SortedFilterBase
    : TemplatedBytecode2<Content, EqualRangeLowerBoundUpperBound> {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_4(uint32_t,
                                     col,
                                     reg::ReadHandle<CastFilterValueResult>,
                                     val_register,
                                     reg::RwHandle<Range>,
                                     update_register,
                                     BoundModifier,
                                     write_result_to);
};

// Specialized filter for sorted data with specific value type and range
// operation.
template <typename T, typename RangeOp>
struct SortedFilter : SortedFilterBase {
  static_assert(TS1::Contains<T>());
  static_assert(TS2::Contains<RangeOp>());
};

// Base for filter operations on non-string values.
struct NonStringFilterBase : TemplatedBytecode2<NonStringContent, NonStringOp> {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_4(uint32_t,
                                     col,
                                     reg::ReadHandle<CastFilterValueResult>,
                                     val_register,
                                     reg::ReadHandle<Slab<uint32_t>>,
                                     source_register,
                                     reg::RwHandle<Slab<uint32_t>>,
                                     update_register);
};

// Specialized filter for non-string data.
template <typename T, typename NonStringOp>
struct NonStringFilter : NonStringFilterBase {
  static_assert(TS1::Contains<T>());
  static_assert(TS2::Contains<NonStringOp>());
};

// Copies data with stride expansion (for joining multiple columns).
struct StrideExpandedCopy : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_3(reg::ReadHandle<Slab<uint32_t>>,
                                     source_register,
                                     reg::RwHandle<Slab<uint32_t>>,
                                     update_register,
                                     uint32_t,
                                     stride);
};

// Clears a register (sets to Empty).
struct ClearRegister : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_1(reg::WriteHandle<reg::Empty>,
                                     dest_register);
};

// List of all bytecode instruction types for variant definition.
#define PERFETTO_DATAFRAME_BYTECODE_LIST(X) \
  X(InitRange)                              \
  X(AllocateIndices)                        \
  X(Iota)                                   \
  X(CastFilterValue<Id>)                    \
  X(SortedFilter<Id, EqualRange>)           \
  X(NonStringFilter<Id, Eq>)                \
  X(StrideExpandedCopy)                     \
  X(ClearRegister)

#define PERFETTO_DATAFRAME_BYTECODE_VARIANT(...) __VA_ARGS__,

// Variant type containing all possible bytecode instructions.
using BytecodeVariant = std::variant<PERFETTO_DATAFRAME_BYTECODE_LIST(
    PERFETTO_DATAFRAME_BYTECODE_VARIANT) std::monostate>;

// Gets the variant index for a specific bytecode type.
template <typename T>
constexpr uint32_t Index() {
  return base::variant_index<BytecodeVariant, T>();
}

// Gets bytecode index for a templated type with one type parameter.
template <template <typename> typename T, typename V1>
PERFETTO_ALWAYS_INLINE constexpr uint32_t Index(const V1& f) {
  using Start = T<typename V1::template GetTypeAtIndex<0>>;
  using End = T<typename V1::template GetTypeAtIndex<V1::kSize - 1>>;
  uint32_t offset = Start::OpcodeOffset(f);
  if (offset > Index<End>() - Index<Start>()) {
    PERFETTO_FATAL("Invalid opcode offset %u (start: %u, end: %u)", offset,
                   Index<Start>(), Index<End>());
  }
  return Index<Start>() + offset;
}

// Gets bytecode index for a templated type with two type parameters.
template <template <typename, typename> typename T, typename V1, typename V2>
PERFETTO_ALWAYS_INLINE constexpr uint32_t Index(const V1& f, const V2& s) {
  using Start = T<typename V1::template GetTypeAtIndex<0>,
                  typename V2::template GetTypeAtIndex<0>>;
  using End = T<typename V1::template GetTypeAtIndex<V1::kSize - 1>,
                typename V2::template GetTypeAtIndex<V2::kSize - 1>>;
  uint32_t offset = Start::OpcodeOffset(f, s);
  if (offset > Index<End>() - Index<Start>()) {
    PERFETTO_FATAL("Invalid opcode offset %u (start: %u, end: %u)", offset,
                   Index<Start>(), Index<End>());
  }
  return Index<Start>() + offset;
}

// Converts a bytecode instruction to string representation.
inline std::string ToString(const Bytecode& op) {
#define PERFETTO_DATAFRAME_BYTECODE_CASE_TO_STRING(...)            \
  case base::variant_index<bytecode::BytecodeVariant,              \
                           bytecode::__VA_ARGS__>(): {             \
    bytecode::__VA_ARGS__ typed_op;                                \
    typed_op.option = op.option;                                   \
    typed_op.args_buffer = op.args_buffer;                         \
    return std::string(#__VA_ARGS__) + ": " + typed_op.ToString(); \
  }
  switch (op.option) {
    PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_CASE_TO_STRING)
    default:
      PERFETTO_FATAL("Unknown opcode %u", op.option);
  }
}

}  // namespace perfetto::trace_processor::dataframe::impl::bytecode

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_BYTECODE_INSTRUCTIONS_H_
