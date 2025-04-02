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
  PERFETTO_DATAFRAME_BYTECODE_IMPL_3(uint32_t,
                                     size,
                                     reg::WriteHandle<Slab<uint32_t>>,
                                     dest_slab_register,
                                     reg::WriteHandle<Span<uint32_t>>,
                                     dest_span_register);
};

// Fills a memory region with sequential integers (0...n-1).
struct Iota : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_2(reg::ReadHandle<Range>,
                                     source_register,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register);
};

// Base class for casting filter value operations.
struct CastFilterValueBase : TemplatedBytecode1<StorageType> {
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
    : TemplatedBytecode2<StorageType, EqualRangeLowerBoundUpperBound> {
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

// Specialized filter for Uint32 columns with SetIdSorted state and equality
// operation.
struct Uint32SetIdSortedEq : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_3(uint32_t,
                                     col,
                                     reg::ReadHandle<CastFilterValueResult>,
                                     val_register,
                                     reg::RwHandle<Range>,
                                     update_register);
};

// Filter operations on non-string columns.
struct NonStringFilterBase : TemplatedBytecode2<NonStringType, NonStringOp> {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_4(uint32_t,
                                     col,
                                     reg::ReadHandle<CastFilterValueResult>,
                                     val_register,
                                     reg::ReadHandle<Span<uint32_t>>,
                                     source_register,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register);
};
template <typename T, typename NonStringOp>
struct NonStringFilter : NonStringFilterBase {
  static_assert(TS1::Contains<T>());
  static_assert(TS2::Contains<NonStringOp>());
};

// Filter operations on string columns.
struct StringFilterBase : TemplatedBytecode1<StringOp> {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_4(uint32_t,
                                     col,
                                     reg::ReadHandle<CastFilterValueResult>,
                                     val_register,
                                     reg::ReadHandle<Span<uint32_t>>,
                                     source_register,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register);
};
template <typename Op>
struct StringFilter : StringFilterBase {
  static_assert(TS1::Contains<Op>());
};

// Copies data with a given stride.
struct StrideCopy : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_3(reg::ReadHandle<Span<uint32_t>>,
                                     source_register,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register,
                                     uint32_t,
                                     stride);
};

// Computes the prefix popcount for the null overlay for a given column.
//
// Popcount means to compute the number of set bits in a word of a BitVector. So
// prefix popcount is a along with a prefix sum over the counts vector.
//
// Note: if `dest_register` already has a value, we'll assume that this bytecode
// has already been executed and skip the computation. This allows for caching
// the result of this bytecode across executions of the interpreter.
struct PrefixPopcount : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_2(uint32_t,
                                     col,
                                     reg::WriteHandle<Slab<uint32_t>>,
                                     dest_register);
};

// Translates a set of indices into a sparse null overlay into indices into
// the underlying storage.
//
// Note that every index in the `source_register` is assumed to be a non-null
// index (i.e. the position of a set bit in the null overlay). To accomplish
// this, make sure to first apply a NullFilter with the IsNotNull operator.
//
// `popcount_register` should point to a register containing the result of the
// PrefixPopcount instruction. This is used to significantly accelerate the
// translation.
struct TranslateSparseNullIndices : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_4(uint32_t,
                                     col,
                                     reg::ReadHandle<Slab<uint32_t>>,
                                     popcount_register,
                                     reg::ReadHandle<Span<uint32_t>>,
                                     source_register,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register);
};

// Base class for null filter operations.
struct NullFilterBase : TemplatedBytecode1<NullOp> {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_2(uint32_t,
                                     col,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register);
};

// Template specialization for a given null operator.
template <typename NullOp>
struct NullFilter : NullFilterBase {
  static_assert(TS1::Contains<NullOp>());
};

// A complex opcode which does the following:
// 1. Iterates over indices in `update_register` starting at offset 0 each
//    incrementing by `stride` each iteration.
// 2. For each such index, if it's non-null, translates it using the sparse null
//    translation logic (see TranslateSparseNullIndices) for the sparse null
//    overlay of `col`
// 3. If the index is null, replace it with UINT32_MAX (representing NULL).
// 4. Copies the result of step 2/3 into position `offset` of the current "row"
//    of indices in `update_register`.
//
// Necessary for the case where we are trying to build the output indices span
// with all the indices into the storage for each relevant column.
struct StrideTranslateAndCopySparseNullIndices : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_5(uint32_t,
                                     col,
                                     reg::ReadHandle<Slab<uint32_t>>,
                                     popcount_register,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register,
                                     uint32_t,
                                     offset,
                                     uint32_t,
                                     stride);
};

// A complex opcode which does the following:
// 1. Iterates over indices in `read_register` starting at offset 0 each
//    incrementing by `stride` each iteration.
// 2. For each such index, if it's non-null, just use it as is in step 4.
// 3. If the index is null, replace it with UINT32_MAX (representing NULL).
// 4. Copies the result of step 2/3 into position `offset` of the current "row"
//    of indices in `update_register`.
//
// Necessary for the case where we are trying to build the output indices span
// with all the indices into the storage for each relevant column.
struct StrideCopyDenseNullIndices : Bytecode {
  PERFETTO_DATAFRAME_BYTECODE_IMPL_4(uint32_t,
                                     col,
                                     reg::RwHandle<Span<uint32_t>>,
                                     update_register,
                                     uint32_t,
                                     offset,
                                     uint32_t,
                                     stride);
};

// List of all bytecode instruction types for variant definition.
#define PERFETTO_DATAFRAME_BYTECODE_LIST(X)  \
  X(InitRange)                               \
  X(AllocateIndices)                         \
  X(Iota)                                    \
  X(CastFilterValue<Id>)                     \
  X(CastFilterValue<Uint32>)                 \
  X(CastFilterValue<Int32>)                  \
  X(CastFilterValue<Int64>)                  \
  X(CastFilterValue<Double>)                 \
  X(CastFilterValue<String>)                 \
  X(SortedFilter<Id, EqualRange>)            \
  X(SortedFilter<Id, LowerBound>)            \
  X(SortedFilter<Id, UpperBound>)            \
  X(SortedFilter<Uint32, EqualRange>)        \
  X(SortedFilter<Uint32, LowerBound>)        \
  X(SortedFilter<Uint32, UpperBound>)        \
  X(SortedFilter<Int32, EqualRange>)         \
  X(SortedFilter<Int32, LowerBound>)         \
  X(SortedFilter<Int32, UpperBound>)         \
  X(SortedFilter<Int64, EqualRange>)         \
  X(SortedFilter<Int64, LowerBound>)         \
  X(SortedFilter<Int64, UpperBound>)         \
  X(SortedFilter<Double, EqualRange>)        \
  X(SortedFilter<Double, LowerBound>)        \
  X(SortedFilter<Double, UpperBound>)        \
  X(SortedFilter<String, EqualRange>)        \
  X(SortedFilter<String, LowerBound>)        \
  X(SortedFilter<String, UpperBound>)        \
  X(Uint32SetIdSortedEq)                     \
  X(NonStringFilter<Id, Eq>)                 \
  X(NonStringFilter<Id, Ne>)                 \
  X(NonStringFilter<Id, Lt>)                 \
  X(NonStringFilter<Id, Le>)                 \
  X(NonStringFilter<Id, Gt>)                 \
  X(NonStringFilter<Id, Ge>)                 \
  X(NonStringFilter<Uint32, Eq>)             \
  X(NonStringFilter<Uint32, Ne>)             \
  X(NonStringFilter<Uint32, Lt>)             \
  X(NonStringFilter<Uint32, Le>)             \
  X(NonStringFilter<Uint32, Gt>)             \
  X(NonStringFilter<Uint32, Ge>)             \
  X(NonStringFilter<Int64, Eq>)              \
  X(NonStringFilter<Int64, Ne>)              \
  X(NonStringFilter<Int64, Lt>)              \
  X(NonStringFilter<Int64, Le>)              \
  X(NonStringFilter<Int64, Gt>)              \
  X(NonStringFilter<Int64, Ge>)              \
  X(NonStringFilter<Double, Eq>)             \
  X(NonStringFilter<Double, Ne>)             \
  X(NonStringFilter<Double, Lt>)             \
  X(NonStringFilter<Double, Le>)             \
  X(NonStringFilter<Double, Gt>)             \
  X(NonStringFilter<Double, Ge>)             \
  X(StringFilter<Eq>)                        \
  X(StringFilter<Ne>)                        \
  X(StringFilter<Lt>)                        \
  X(StringFilter<Le>)                        \
  X(StringFilter<Gt>)                        \
  X(StringFilter<Ge>)                        \
  X(StringFilter<Glob>)                      \
  X(StringFilter<Regex>)                     \
  X(NullFilter<IsNotNull>)                   \
  X(NullFilter<IsNull>)                      \
  X(StrideCopy)                              \
  X(StrideTranslateAndCopySparseNullIndices) \
  X(StrideCopyDenseNullIndices)              \
  X(PrefixPopcount)                          \
  X(TranslateSparseNullIndices)

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
