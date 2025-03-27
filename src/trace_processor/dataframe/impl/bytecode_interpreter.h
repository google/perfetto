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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_BYTECODE_INTERPRETER_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_BYTECODE_INTERPRETER_H_

#include <alloca.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <type_traits>
#include <utility>
#include <variant>
#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/public/compiler.h"

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/impl/bytecode_core.h"
#include "src/trace_processor/dataframe/impl/bytecode_instructions.h"
#include "src/trace_processor/dataframe/impl/bytecode_registers.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode {

// Returns an appropriate comparator functor for the given numeric type and
// operation. Currently only supports equality comparison.
template <typename T, typename Op>
auto NumericComparator() {
  if constexpr (std::is_same_v<Op, Eq>) {
    return std::equal_to<T>();
  } else {
    static_assert(false);
  }
}

// Handles invalid cast filter value results for filtering operations.
// If the cast result is invalid, updates the range or segment accordingly.
template <typename T>
PERFETTO_ALWAYS_INLINE bool HandleInvalidCastFilterValueResult(
    const CastFilterValueResult& value,
    T& update) {
  static_assert(std::is_same_v<T, Range> || std::is_same_v<T, Span<uint32_t>>);
  if (PERFETTO_UNLIKELY(value.validity != CastFilterValueResult::kValid)) {
    if (value.validity == CastFilterValueResult::kNoneMatch) {
      update.e = update.b;
    }
    return false;
  }
  return true;
}

// The Interpreter class implements a virtual machine that executes bytecode
// instructions for dataframe query operations. It maintains an internal
// register state, processes sequences of bytecode operations, and applies
// filter and transformation operations to data columns. The interpreter is
// designed for high-performance data filtering and manipulation, with
// specialized handling for different data types and comparison operations.
class Interpreter {
 public:
  Interpreter(BytecodeVector bytecode,
              const FilterSpec::Value* filter_values,
              const Column* columns,
              const StringPool* spool)
      : bytecode_(std::move(bytecode)),
        filter_values_(filter_values),
        columns_(columns),
        spool_(spool) {
    base::ignore_result(spool_);
  }

  // Not movable because it's a very large object and the move cost would be
  // high. Phandleer constructing in place.
  Interpreter(Interpreter&&) = delete;
  Interpreter& operator=(Interpreter&&) = delete;

#define PERFETTO_DATAFRAME_BYTECODE_CASE_FN(...)                            \
  case base::variant_index<bytecode::BytecodeVariant,                       \
                           bytecode::__VA_ARGS__>(): {                      \
    this->__VA_ARGS__(static_cast<const bytecode::__VA_ARGS__&>(bytecode)); \
    break;                                                                  \
  }

  // Executes the bytecode sequence and returns the result stored in the
  // specified output register. Processes each bytecode instruction in sequence,
  // dispatching to the appropriate handler.
  void Execute() {
    for (const auto& bytecode : bytecode_) {
      switch (bytecode.option) {
        PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_CASE_FN)
        default:
          PERFETTO_ASSUME(false);
      }
    }
  }

  // Returns the value of the specified register if it contains the expected
  // type. Returns nullptr if the register holds a different type or is empty.
  template <typename T>
  const T* GetRegisterValue(reg::ReadHandle<T> reg) {
    if (std::holds_alternative<T>(registers_[reg.index])) {
      return &base::unchecked_get<T>(registers_[reg.index]);
    }
    return nullptr;
  }

  // Sets the value of the specified register for testing purposes.
  //
  // Makes it easier to test certain bytecode instructions which depend on
  // the preexisting value of a register.
  template <typename T>
  void SetRegisterValueForTesting(reg::WriteHandle<T> reg, T value) {
    WriteReg(reg, std::move(value));
  }

  const Column* columns() const { return columns_; }

 private:
  // Initializes a range with the specified size, starting at zero.
  PERFETTO_ALWAYS_INLINE void InitRange(const bytecode::InitRange& init) {
    using B = bytecode::InitRange;
    WriteReg(init.arg<B::dest_register>(), Range{0, init.arg<B::size>()});
  }

  PERFETTO_ALWAYS_INLINE void AllocateIndices(
      const bytecode::AllocateIndices& ai) {
    using B = bytecode::AllocateIndices;

    if (auto* exist_slab = MaybeReg(ai.arg<B::dest_slab_register>())) {
      // Ensure that the slab is the same size as the requested size.
      PERFETTO_DCHECK(exist_slab->size() == ai.arg<B::size>());

      // Update the span to point to the pre-allocated slab.
      WriteReg(ai.arg<B::dest_span_register>(),
               Span<uint32_t>{exist_slab->begin(), exist_slab->end()});
    } else {
      auto slab = Slab<uint32_t>::Alloc(ai.arg<B::size>());
      Span<uint32_t> span{slab.begin(), slab.end()};
      WriteReg(ai.arg<B::dest_slab_register>(), std::move(slab));
      WriteReg(ai.arg<B::dest_span_register>(), span);
    }
  }

  // Fills a SlabSegment with sequential values starting from source.begin().
  PERFETTO_ALWAYS_INLINE void Iota(const bytecode::Iota& r) {
    using B = bytecode::Iota;
    const auto& source = Reg(r.arg<B::source_register>());

    auto& update = Reg(r.arg<B::update_register>());
    PERFETTO_DCHECK(source.size() <= update.size());
    auto* end = update.b + source.size();
    std::iota(update.b, end, source.b);
    update.e = end;
  }

  // Attempts to cast a filter value to the specified type and stores the
  // result. Currently only supports casting to Id type.
  template <typename T>
  PERFETTO_ALWAYS_INLINE void CastFilterValue(
      const bytecode::CastFilterValueBase& f) {
    using B = bytecode::CastFilterValueBase;
    const auto& value = filter_values_[f.arg<B::fval_handle>().index];

    using M = std::variant_alternative_t<Content::GetTypeIndex<T>(),
                                         CastFilterValueResult::Value>;
    CastFilterValueResult::Validity validity;
    M out;
    if constexpr (std::is_same_v<T, Id>) {
      uint32_t val;
      validity = CastFilterValueToInteger(value, f.arg<B::op>(), val);
      out = CastFilterValueResult::Id{val};
    } else {
      static_assert(false, "Unsupported type");
    }
    CastFilterValueResult result;
    result.validity = validity;
    if (validity == CastFilterValueResult::kValid) {
      result.value = out;
    }
    WriteReg(f.arg<B::write_register>(), result);
  }

  // Applies a filter operation to a sorted range based on the provided value.
  // Currently only supports EqualRange operation on Id type.
  template <typename T, typename RangeOp>
  PERFETTO_ALWAYS_INLINE void SortedFilter(
      const bytecode::SortedFilterBase& f) {
    using B = bytecode::SortedFilterBase;

    const CastFilterValueResult& value = Reg(f.arg<B::val_register>());
    auto& update = Reg(f.arg<B::update_register>());
    if (!HandleInvalidCastFilterValueResult(value, update)) {
      return;
    }
    using M = Content::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
    M val = base::unchecked_get<M>(value.value);
    if constexpr (std::is_same_v<T, Id>) {
      uint32_t inner_val = val.value;
      if constexpr (std::is_same_v<RangeOp, EqualRange>) {
        bool in_bounds = inner_val >= update.b && inner_val < update.e;
        update.b = inner_val;
        update.e = inner_val + in_bounds;
      } else {
        static_assert(false, "Unsupported op");
      }
    } else {
      static_assert(false, "Unsupported type");
    }
  }

  // Applies a non-string filter operation to a range of values.
  // Currently only supports equality filtering on Id type.
  template <typename T, typename Op>
  PERFETTO_ALWAYS_INLINE void NonStringFilter(
      const bytecode::NonStringFilterBase& nf) {
    using B = bytecode::NonStringFilter<T, Op>;
    const CastFilterValueResult& value = Reg(nf.arg<B::val_register>());
    auto& update = Reg(nf.arg<B::update_register>());
    if (!HandleInvalidCastFilterValueResult(value, update)) {
      return;
    }
    const auto& source = Reg(nf.arg<B::source_register>());
    using M = Content::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
    if constexpr (std::is_same_v<T, Id>) {
      update.e = IdentityFilter(source.b, source.e, update.b,
                                base::unchecked_get<M>(value.value).value,
                                NumericComparator<uint32_t, Op>());
    }
  }

  // Copies values from source to update with a specified stride.
  PERFETTO_ALWAYS_INLINE void StrideCopy(const bytecode::StrideCopy& tr) {
    using B = bytecode::StrideCopy;
    const auto& source = Reg(tr.arg<B::source_register>());
    auto& update = Reg(tr.arg<B::update_register>());
    uint32_t stride = tr.arg<B::stride>();
    PERFETTO_DCHECK(source.size() * stride <= update.size());
    uint32_t* ptr = update.b;
    for (const uint32_t* it = source.b; it < source.e; ++it) {
      *ptr = *it;
      ptr += stride;
    }
    PERFETTO_DCHECK(ptr == update.b + source.size() * stride);
    update.e = ptr;
  }

  // Filters data based on a comparison with a specific value.
  // Only copies values that match the comparison condition.
  template <typename Comparator, typename V, typename I>
  [[nodiscard]] PERFETTO_ALWAYS_INLINE static uint32_t* Filter(
      const I* data,
      const uint32_t* begin,
      const uint32_t* end,
      uint32_t* o_start,
      const V& value,
      const Comparator& comparator) {
    uint32_t* o_read = o_start;
    uint32_t* o_write = o_start;
    for (const uint32_t* it = begin; it != end; ++it, ++o_read) {
      *o_write = *o_read;
      o_write += comparator(data[*it], value);
    }
    return o_write;
  }

  // Similar to Filter but operates directly on the identity values (indices)
  // rather than dereferencing through a data array.
  template <typename Comparator>
  [[nodiscard]] PERFETTO_ALWAYS_INLINE static uint32_t* IdentityFilter(
      const uint32_t* begin,
      const uint32_t* end,
      uint32_t* o_start,
      uint32_t value,
      Comparator comparator) {
    uint32_t* o_read = o_start;
    uint32_t* o_write = o_start;
    for (const uint32_t* it = begin; it != end; ++it, ++o_read) {
      *o_write = *o_read;
      o_write += comparator(*it, value);
    }
    return o_write;
  }

  // Attempts to cast a filter value to a numeric type, dispatching to the
  // appropriate type-specific conversion function.
  template <typename T>
  [[nodiscard]] PERFETTO_ALWAYS_INLINE static CastFilterValueResult::Validity
  CastFilterValueToNumeric(const FilterSpec::Value& value,
                           NonNullOp op,
                           T& out) {
    if constexpr (std::is_same_v<T, double>) {
      return CastFilterValueToDouble(value, op, out);
    } else if constexpr (std::is_integral_v<T>) {
      return CastFilterValueToInteger<T>(value, op, out);
    } else if constexpr (std::is_same_v<T, uint32_t>) {
    } else {
      static_assert(false);
    }
  }

  // Attempts to cast a filter value to an integer type, handling various edge
  // cases such as out-of-range values and non-integer inputs.
  template <typename T>
  [[nodiscard]] PERFETTO_ALWAYS_INLINE static CastFilterValueResult::Validity
  CastFilterValueToInteger(const FilterSpec::Value& value,
                           NonNullOp op,
                           T& out) {
    using Op = NonNullOp;
    static_assert(std::is_integral_v<T>);
    if (PERFETTO_LIKELY(std::holds_alternative<int64_t>(value))) {
      int64_t res = base::unchecked_get<int64_t>(value);
      bool is_small = res < std::numeric_limits<T>::min();
      bool is_big = res > std::numeric_limits<T>::max();
      if (PERFETTO_UNLIKELY(is_small || is_big)) {
        switch (op.index()) {
          case Op::GetTypeIndex<Eq>():
            return CastFilterValueResult::kNoneMatch;
          default:
            PERFETTO_FATAL("Invalid numeric filter op");
        }
        return CastFilterValueResult::kAllMatch;
      }
      out = static_cast<T>(res);
      return CastFilterValueResult::kValid;
    }
    if (PERFETTO_LIKELY(std::holds_alternative<double>(value))) {
      double d = base::unchecked_get<double>(value);
      // We use the constants directly instead of using numeric_limits for
      // int64_t as the casts introduces rounding in the doubles as a double
      // cannot exactly represent int64::max().
      constexpr double kMin = std::is_same_v<T, int64_t>
                                  ? -9223372036854775808.0
                                  : std::numeric_limits<T>::min();
      constexpr double kMax = std::is_same_v<T, int64_t>
                                  ? 9223372036854775808.0
                                  : std::numeric_limits<T>::max();

      // NaNs always compare false to any value (including other NaNs),
      // regardless of the operator.
      if (PERFETTO_UNLIKELY(std::isnan(d))) {
        return CastFilterValueResult::kNoneMatch;
      }

      bool is_small = d < kMin;
      bool is_big = d > kMax;
      if (PERFETTO_LIKELY(d == trunc(d) && !is_small && !is_big)) {
        out = static_cast<T>(d);
        return CastFilterValueResult::kValid;
      }

      switch (op.index()) {
        case NonStringOp::GetTypeIndex<Eq>():
          return CastFilterValueResult::kNoneMatch;
        default:
          PERFETTO_FATAL("Invalid numeric filter op");
      }
    }
    return NumericConvertNonNumericValue(value, op);
  }

  // Attempts to cast a filter value to a double, handling integer inputs and
  // various edge cases.
  [[nodiscard]] PERFETTO_ALWAYS_INLINE static CastFilterValueResult::Validity
  CastFilterValueToDouble(const FilterSpec::Value& value,
                          NonNullOp op,
                          double& out) {
    using Op = NonStringOp;
    if (PERFETTO_LIKELY(std::holds_alternative<double>(value))) {
      out = base::unchecked_get<double>(value);
      return CastFilterValueResult::kValid;
    }
    if (PERFETTO_LIKELY(std::holds_alternative<int64_t>(value))) {
      int64_t i = base::unchecked_get<int64_t>(value);
      auto iad = static_cast<double>(i);
      int64_t iad_int = static_cast<int64_t>(iad);
      if (i == iad_int) {
        out = iad;
        return CastFilterValueResult::kValid;
      }
      switch (op.index()) {
        case Op::GetTypeIndex<Eq>():
          return CastFilterValueResult::kNoneMatch;
        default:
          PERFETTO_FATAL("Invalid numeric filter op");
      }
    }
    return NumericConvertNonNumericValue(value, op);
  }

  // Converts a double to an integer type using the specified function (e.g.,
  // trunc, floor). Used as a helper for various numeric conversion operations.
  template <typename T, double (*fn)(double)>
  PERFETTO_ALWAYS_INLINE static CastFilterValueResult::Validity
  DoubleToInt(bool no_data, bool all_data, double d, T& out) {
    if (no_data) {
      return CastFilterValueResult::kNoneMatch;
    }
    if (all_data) {
      return CastFilterValueResult::kAllMatch;
    }
    out = static_cast<T>(fn(d));
    return CastFilterValueResult::kValid;
  }

  // Handles conversion of non-numeric values (strings, nulls) to numeric types
  // for comparison operations.
  PERFETTO_ALWAYS_INLINE static CastFilterValueResult::Validity
  NumericConvertNonNumericValue(const FilterSpec::Value& value, NonNullOp op) {
    if (std::holds_alternative<const char*>(value)) {
      using Op = NonStringOp;
      if (op.index() == Op::GetTypeIndex<Eq>()) {
        return CastFilterValueResult::kNoneMatch;
      }
      PERFETTO_DCHECK(false);
      return CastFilterValueResult::kAllMatch;
    }

    PERFETTO_DCHECK(std::holds_alternative<nullptr_t>(value));

    // Nulls always compare false to any value (including other nulls),
    // regardless of the operator.
    return CastFilterValueResult::kNoneMatch;
  }

  // Writes a value to the specified register, handling type safety through the
  // handle.
  template <typename T>
  PERFETTO_ALWAYS_INLINE void WriteReg(reg::WriteHandle<T> reg, T value) {
    registers_[reg.index] = std::move(value);
  }

  // Access a register for reading/writing with type safety through the handle.
  template <typename T>
  PERFETTO_ALWAYS_INLINE T& Reg(reg::RwHandle<T> reg) {
    return base::unchecked_get<T>(registers_[reg.index]);
  }

  // Access a register for reading only with type safety through the handle.
  template <typename T>
  PERFETTO_ALWAYS_INLINE const T& Reg(reg::ReadHandle<T> reg) const {
    return base::unchecked_get<T>(registers_[reg.index]);
  }

  // Alternative overload for accessing a read/write register as read-only.
  template <typename T>
  PERFETTO_ALWAYS_INLINE const T& Reg(reg::RwHandle<T> reg) const {
    return base::unchecked_get<T>(registers_[reg.index]);
  }

  // Conditionally access a register if it contains the expected type.
  // Returns nullptr if the register holds a different type.
  template <typename T>
  PERFETTO_ALWAYS_INLINE T* MaybeReg(const reg::WriteHandle<T>& reg) {
    if (std::holds_alternative<T>(registers_[reg.index])) {
      return &base::unchecked_get<T>(registers_[reg.index]);
    }
    return nullptr;
  }

  // The sequence of bytecode instructions to execute
  BytecodeVector bytecode_;
  // Register file holding intermediate values
  std::array<reg::Value, reg::kMaxRegisters> registers_;

  // Pointer to the filter values for comparison operations
  const FilterSpec::Value* filter_values_;
  // Pointer to the data columns being processed
  const Column* columns_;
  // Pointer to the string pool (for string operations)
  const StringPool* spool_;
};

}  // namespace perfetto::trace_processor::dataframe::impl::bytecode

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_BYTECODE_INTERPRETER_H_
