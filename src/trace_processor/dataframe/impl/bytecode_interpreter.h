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
#include "src/trace_processor/dataframe/value_fetcher.h"

namespace perfetto::trace_processor::dataframe::impl::bytecode {

// Returns an appropriate comparator functor for the given numeric type and
// operation. Currently only supports equality comparison.
template <typename T, typename Op>
auto NumericComparator() {
  if constexpr (std::is_same_v<Op, Eq>) {
    return std::equal_to<T>();
  } else if constexpr (std::is_same_v<Op, Ne>) {
    return std::not_equal_to<T>();
  } else if constexpr (std::is_same_v<Op, Lt>) {
    return std::less<T>();
  } else if constexpr (std::is_same_v<Op, Le>) {
    return std::less_equal<T>();
  } else if constexpr (std::is_same_v<Op, Gt>) {
    return std::greater<T>();
  } else if constexpr (std::is_same_v<Op, Ge>) {
    return std::greater_equal<T>();
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
template <typename FVF>
class Interpreter {
 public:
  static_assert(std::is_base_of_v<ValueFetcher, FVF>,
                "FVF must be a subclass of ValueFetcher");

  Interpreter(BytecodeVector bytecode,
              const Column* columns,
              const StringPool* spool)
      : bytecode_(std::move(bytecode)), columns_(columns), spool_(spool) {
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
  PERFETTO_ALWAYS_INLINE void Execute(FVF& fvf) {
    fvf_ = &fvf;
    for (const auto& bytecode : bytecode_) {
      switch (bytecode.option) {
        PERFETTO_DATAFRAME_BYTECODE_LIST(PERFETTO_DATAFRAME_BYTECODE_CASE_FN)
        default:
          PERFETTO_ASSUME(false);
      }
    }
    fvf_ = nullptr;
  }

  // Returns the value of the specified register if it contains the expected
  // type. Returns nullptr if the register holds a different type or is empty.
  template <typename T>
  PERFETTO_ALWAYS_INLINE const T* GetRegisterValue(reg::ReadHandle<T> reg) {
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
    FilterValueHandle h = f.arg<B::fval_handle>();
    auto filter_value_type = fvf_->ValueType(h.index);

    using M = std::variant_alternative_t<ColumnType::GetTypeIndex<T>(),
                                         CastFilterValueResult::Value>;
    CastFilterValueResult result;
    if constexpr (std::is_same_v<T, Id>) {
      uint32_t val;
      result.validity = CastFilterValueToInteger(h, filter_value_type, fvf_,
                                                 f.arg<B::op>(), val);
      if (PERFETTO_LIKELY(result.validity == CastFilterValueResult::kValid)) {
        result.value = CastFilterValueResult::Id{val};
      }
    } else if constexpr (NumericType::Contains<T>()) {
      M out;
      result.validity = CastFilterValueToNumeric(h, filter_value_type, fvf_,
                                                 f.arg<B::op>(), out);
      if (PERFETTO_LIKELY(result.validity == CastFilterValueResult::kValid)) {
        result.value = out;
      }
    } else {
      static_assert(std::is_same_v<T, Id>, "Unsupported type");
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
    using M = ColumnType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
    M val = base::unchecked_get<M>(value.value);
    if constexpr (std::is_same_v<T, Id>) {
      uint32_t inner_val = val.value;
      if constexpr (std::is_same_v<RangeOp, EqualRange>) {
        bool in_bounds = inner_val >= update.b && inner_val < update.e;
        update.b = inner_val;
        update.e = inner_val + in_bounds;
      } else if constexpr (std::is_same_v<RangeOp, LowerBound> ||
                           std::is_same_v<RangeOp, UpperBound>) {
        if (inner_val >= update.b && inner_val < update.e) {
          BoundModifier bound = f.arg<B::write_result_to>();
          auto& res = bound.Is<BeginBound>() ? update.b : update.e;
          res = inner_val + std::is_same_v<RangeOp, UpperBound>;
        } else {
          update.e = update.b;
        }
      } else {
        static_assert(std::is_same_v<RangeOp, EqualRange>, "Unsupported op");
      }
    } else if constexpr (NumericType::Contains<T>()) {
      BoundModifier b = f.arg<B::write_result_to>();
      auto* d = columns_[f.arg<B::col>()].storage.template unchecked_data<T>();
      SortedNumericFilter<RangeOp>(d + update.b, d + update.e, val, b, update);
    } else {
      static_assert(std::is_same_v<T, Id>, "Unsupported type");
    }
  }

  template <typename RangeOp, typename M>
  PERFETTO_ALWAYS_INLINE void SortedNumericFilter(const M* begin,
                                                  const M* end,
                                                  M val,
                                                  BoundModifier bound,
                                                  Range& update) {
    if constexpr (std::is_same_v<RangeOp, EqualRange>) {
      PERFETTO_DCHECK(bound.Is<BothBounds>());
      const M* eq_start = std::lower_bound(begin, end, val);
      for (auto* it = eq_start; it != end; ++it) {
        if (*it != val) {
          update.b = eq_start - begin;
          update.e = it - begin;
          return;
        }
      }
      update.e = update.b;
    } else if constexpr (std::is_same_v<RangeOp, LowerBound>) {
      auto& res = bound.Is<BeginBound>() ? update.b : update.e;
      res = std::lower_bound(begin, end, val) - begin;
    } else if constexpr (std::is_same_v<RangeOp, UpperBound>) {
      auto& res = bound.Is<BeginBound>() ? update.b : update.e;
      res = std::upper_bound(begin, end, val) - begin;
    } else {
      static_assert(std::is_same_v<RangeOp, EqualRange>, "Unsupported op");
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
    using M = ColumnType::VariantTypeAtIndex<T, CastFilterValueResult::Value>;
    if constexpr (std::is_same_v<T, Id>) {
      update.e = IdentityFilter(source.b, source.e, update.b,
                                base::unchecked_get<M>(value.value).value,
                                NumericComparator<uint32_t, Op>());
    } else if constexpr (NumericType::Contains<T>()) {
      const auto* data =
          columns_[nf.arg<B::col>()].storage.template unchecked_data<T>();
      update.e = Filter(data, source.b, source.e, update.b,
                        base::unchecked_get<M>(value.value),
                        NumericComparator<M, Op>());
    } else {
      static_assert(std::is_same_v<T, Id>, "Unsupported type");
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
    uint32_t* o_write = o_start;
    for (const uint32_t* it = begin; it != end; ++it) {
      *o_write = *it;
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
  CastFilterValueToNumeric(FilterValueHandle handle,
                           typename FVF::Type filter_value_type,
                           FVF* fetcher,
                           NonNullOp op,
                           T& out) {
    if constexpr (std::is_same_v<T, double>) {
      return CastFilterValueToDouble(handle, filter_value_type, fetcher, op,
                                     out);
    } else if constexpr (std::is_integral_v<T>) {
      return CastFilterValueToInteger<T>(handle, filter_value_type, fetcher, op,
                                         out);
    } else {
      static_assert(std::is_same_v<T, double>, "Unsupported type");
    }
  }

  // Attempts to cast a filter value to an integer type, handling various edge
  // cases such as out-of-range values and non-integer inputs.
  template <typename T>
  [[nodiscard]] PERFETTO_ALWAYS_INLINE static CastFilterValueResult::Validity
  CastFilterValueToInteger(FilterValueHandle handle,
                           typename FVF::Type filter_value_type,
                           FVF* fvf,
                           NonStringOp op,
                           T& out) {
    static_assert(std::is_integral_v<T>, "Unsupported type");

    using Op = NonNullOp;
    if (PERFETTO_LIKELY(filter_value_type == FVF::kInt64)) {
      int64_t res = fvf->Int64Value(handle.index);
      bool is_small = res < std::numeric_limits<T>::min();
      bool is_big = res > std::numeric_limits<T>::max();
      if (PERFETTO_UNLIKELY(is_small || is_big)) {
        switch (op.index()) {
          case Op::GetTypeIndex<Lt>():
          case Op::GetTypeIndex<Le>():
            if (is_small) {
              return CastFilterValueResult::kNoneMatch;
            }
            break;
          case Op::GetTypeIndex<Gt>():
          case Op::GetTypeIndex<Ge>():
            if (is_big) {
              return CastFilterValueResult::kNoneMatch;
            }
            break;
          case Op::GetTypeIndex<Eq>():
            return CastFilterValueResult::kNoneMatch;
          case Op::GetTypeIndex<Ne>():
            // Do nothing.
            break;
          default:
            PERFETTO_FATAL("Invalid numeric filter op");
        }
        return CastFilterValueResult::kAllMatch;
      }
      out = static_cast<T>(res);
      return CastFilterValueResult::kValid;
    }
    if (PERFETTO_LIKELY(filter_value_type == FVF::kDouble)) {
      double d = fvf->DoubleValue(handle.index);

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

      // The greater than or equal is intentional to account for the fact that
      // twos-complement integers are not symmetric around zero (i.e.
      // -9223372036854775808 can be represented but 9223372036854775808
      // cannot).
      bool is_big = d >= kMax;
      bool is_small = d < kMin;
      if (PERFETTO_LIKELY(d == trunc(d) && !is_small && !is_big)) {
        out = static_cast<T>(d);
        return CastFilterValueResult::kValid;
      }

      switch (op.index()) {
        case Op::GetTypeIndex<Lt>():
          return DoubleToInt<T, std::ceil>(is_small, is_big, d, out);
        case Op::GetTypeIndex<Le>():
          return DoubleToInt<T, std::floor>(is_small, is_big, d, out);
        case Op::GetTypeIndex<Gt>():
          return DoubleToInt<T, std::ceil>(is_big, is_small, d, out);
        case Op::GetTypeIndex<Ge>():
          return DoubleToInt<T, std::floor>(is_big, is_small, d, out);
        case Op::GetTypeIndex<Eq>():
          return CastFilterValueResult::kNoneMatch;
        case Op::GetTypeIndex<Ne>():
          // Do nothing.
          return CastFilterValueResult::kAllMatch;
        default:
          PERFETTO_FATAL("Invalid numeric filter op");
      }
    }
    return NumericConvertNonNumericValue(filter_value_type, op);
  }

  // Attempts to cast a filter value to a double, handling integer inputs and
  // various edge cases.
  [[nodiscard]] PERFETTO_ALWAYS_INLINE static CastFilterValueResult::Validity
  CastFilterValueToDouble(FilterValueHandle handle,
                          typename FVF::Type filter_value_type,
                          FVF* fetcher,
                          NonStringOp op,
                          double& out) {
    using Op = NonStringOp;
    if (PERFETTO_LIKELY(filter_value_type == FVF::kDouble)) {
      out = fetcher->DoubleValue(handle.index);
      return CastFilterValueResult::kValid;
    }
    if (PERFETTO_LIKELY(filter_value_type == FVF::kInt64)) {
      int64_t i = fetcher->Int64Value(handle.index);
      auto iad = static_cast<double>(i);
      auto iad_int = static_cast<int64_t>(iad);
      if (PERFETTO_LIKELY(i == iad_int)) {
        out = iad;
        return CastFilterValueResult::kValid;
      }
      switch (op.index()) {
        case Op::GetTypeIndex<Lt>():
          out = iad_int > i ? iad
                            : std::nextafter(
                                  iad, std::numeric_limits<double>::infinity());
          return CastFilterValueResult::kValid;
        case Op::GetTypeIndex<Le>():
          out = iad_int < i
                    ? iad
                    : std::nextafter(iad,
                                     -std::numeric_limits<double>::infinity());
          return CastFilterValueResult::kValid;
        case Op::GetTypeIndex<Gt>():
          out = iad_int > i ? iad
                            : std::nextafter(
                                  iad, std::numeric_limits<double>::infinity());
          return CastFilterValueResult::kValid;
        case Op::GetTypeIndex<Ge>():
          out = iad_int < i
                    ? iad
                    : std::nextafter(iad,
                                     -std::numeric_limits<double>::infinity());
          return CastFilterValueResult::kValid;
        case Op::GetTypeIndex<Eq>():
          return CastFilterValueResult::kNoneMatch;
        case Op::GetTypeIndex<Ne>():
          // Do nothing.
          return CastFilterValueResult::kAllMatch;
        default:
          PERFETTO_FATAL("Invalid numeric filter op");
      }
    }
    return NumericConvertNonNumericValue(filter_value_type, op);
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
  NumericConvertNonNumericValue(typename FVF::Type filter_value_type,
                                NonStringOp op) {
    if (filter_value_type == FVF::kString) {
      using Op = NonStringOp;
      if (op.index() == Op::GetTypeIndex<Eq>() ||
          op.index() == Op::GetTypeIndex<Ge>() ||
          op.index() == Op::GetTypeIndex<Gt>()) {
        return CastFilterValueResult::kNoneMatch;
      }
      PERFETTO_DCHECK(op.index() == Op::GetTypeIndex<Ne>() ||
                      op.index() == Op::GetTypeIndex<Le>() ||
                      op.index() == Op::GetTypeIndex<Lt>());
      return CastFilterValueResult::kAllMatch;
    }

    PERFETTO_DCHECK(filter_value_type == FVF::kNull);

    // Nulls always compare false to any value (including other nulls),
    // regardless of the operator.
    return CastFilterValueResult::kNoneMatch;
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

  // Conditionally access a register if it contains the expected type.
  // Returns nullptr if the register holds a different type.
  template <typename T>
  PERFETTO_ALWAYS_INLINE T* MaybeReg(reg::WriteHandle<T> reg) {
    if (std::holds_alternative<T>(registers_[reg.index])) {
      return &base::unchecked_get<T>(registers_[reg.index]);
    }
    return nullptr;
  }

  // Writes a value to the specified register, handling type safety through the
  // handle.
  template <typename T>
  PERFETTO_ALWAYS_INLINE void WriteReg(reg::WriteHandle<T> reg, T value) {
    registers_[reg.index] = std::move(value);
  }

  // The sequence of bytecode instructions to execute
  BytecodeVector bytecode_;
  // Register file holding intermediate values
  std::array<reg::Value, reg::kMaxRegisters> registers_;

  // Pointer to the source for filter values.
  FVF* fvf_;
  // Pointer to the data columns being processed
  const Column* columns_;
  // Pointer to the string pool (for string operations)
  const StringPool* spool_;
};

}  // namespace perfetto::trace_processor::dataframe::impl::bytecode

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_IMPL_BYTECODE_INTERPRETER_H_
