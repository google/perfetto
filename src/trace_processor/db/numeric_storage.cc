
/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <variant>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/numeric_storage.h"

namespace perfetto {
namespace trace_processor {
namespace column {

namespace {

// Templated part of FastPathComparison.
template <typename T>
inline void TypedFastPathComparison(std::optional<NumericValue> val,
                                    FilterOp op,
                                    const T* start,
                                    uint32_t num_elements,
                                    BitVector::Builder& builder) {
  if (!val) {
    builder.Skip(num_elements);
    return;
  }
  std::visit(
      [val, start, num_elements, &builder](auto comparator) {
        T typed_val = std::get<T>(*val);
        for (uint32_t i = 0; i < num_elements; i += BitVector::kBitsInWord) {
          uint64_t word = 0;
          // This part should be optimised by SIMD and is expected to be fast.
          for (uint32_t k = 0; k < BitVector::kBitsInWord; ++k) {
            bool comp_result = comparator(start[i + k], typed_val);
            word |= static_cast<uint64_t>(comp_result) << k;
          }
          builder.AppendWord(word);
        }
      },
      GetFilterOpVariant<T>(op));
}

// Templated part of SlowPathComparison.
template <typename T>
inline void TypedSlowPathComparison(std::optional<NumericValue> val,
                                    FilterOp op,
                                    const T* start,
                                    uint32_t num_elements,
                                    BitVector::Builder& builder) {
  if (!val) {
    builder.Skip(num_elements);
    return;
  }
  std::visit(
      [val, start, num_elements, &builder](auto comparator) {
        T typed_val = std::get<T>(*val);
        for (uint32_t i = 0; i < num_elements; ++i) {
          builder.Append(comparator(start[i], typed_val));
        }
      },
      GetFilterOpVariant<T>(op));
}

}  // namespace

void NumericStorage::StableSort(uint32_t* rows, uint32_t rows_size) const {
  NumericValue val = *GetNumericTypeVariant(type_, SqlValue::Long(0));
  std::visit(
      [this, &rows, rows_size](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data_);
        std::stable_sort(rows, rows + rows_size,
                         [typed_start](uint32_t a_idx, uint32_t b_idx) {
                           T first_val = typed_start[a_idx];
                           T second_val = typed_start[b_idx];
                           return first_val < second_val;
                         });
      },
      val);
}

// Responsible for invoking templated version of FastPathComparison.
void NumericStorage::CompareFast(FilterOp op,
                                 SqlValue sql_val,
                                 uint32_t offset,
                                 uint32_t num_elements,
                                 BitVector::Builder& builder) const {
  PERFETTO_DCHECK(num_elements % BitVector::kBitsInWord == 0);
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);

  // If the value is invalid we should just ignore those elements.
  if (!val.has_value() || op == FilterOp::kIsNotNull ||
      op == FilterOp::kIsNull || op == FilterOp::kGlob) {
    builder.Skip(num_elements);
    return;
  }
  std::visit(
      [this, op, offset, num_elements, &builder](auto num_val) {
        using T = decltype(num_val);
        auto* typed_start = static_cast<const T*>(data_) + offset;
        TypedFastPathComparison(num_val, op, typed_start, num_elements,
                                builder);
      },
      *val);
}

// Responsible for invoking templated version of SlowPathComparison.
void NumericStorage::CompareSlow(FilterOp op,
                                 SqlValue sql_val,
                                 uint32_t offset,
                                 uint32_t num_elements,
                                 BitVector::Builder& builder) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);

  // If the value is invalid we should just ignore those elements.
  if (!val.has_value() || op == FilterOp::kIsNotNull ||
      op == FilterOp::kIsNull || op == FilterOp::kGlob) {
    builder.Skip(num_elements);
    return;
  }

  std::visit(
      [this, op, offset, num_elements, &builder](auto val) {
        using T = decltype(val);
        auto* typed_start = static_cast<const T*>(data_) + offset;
        TypedSlowPathComparison(val, op, typed_start, num_elements, builder);
      },
      *val);
}

uint32_t NumericStorage::UpperBoundIndex(NumericValue val) const {
  return std::visit(
      [this](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data_);
        auto upper =
            std::upper_bound(typed_start, typed_start + size_, val_data);
        return static_cast<uint32_t>(std::distance(typed_start, upper));
      },
      val);
}

// As we don't template those functions, we need to use std::visitor to type
// `start`, hence this wrapping.
uint32_t NumericStorage::LowerBoundIndex(NumericValue val) const {
  return std::visit(
      [this](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data_);
        auto lower =
            std::lower_bound(typed_start, typed_start + size_, val_data);
        return static_cast<uint32_t>(std::distance(typed_start, lower));
      },
      val);
}

void NumericStorage::CompareSorted(FilterOp op,
                                   SqlValue sql_val,
                                   RowMap& rm) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);
  if (!val.has_value() || op == FilterOp::kIsNotNull ||
      op == FilterOp::kIsNull || op == FilterOp::kGlob) {
    rm.Clear();
    return;
  }

  switch (op) {
    case FilterOp::kEq: {
      uint32_t beg = LowerBoundIndex(*val);
      uint32_t end = UpperBoundIndex(*val);
      RowMap sec(beg, end);
      rm.Intersect(sec);
      return;
    }
    case FilterOp::kLe: {
      uint32_t end = UpperBoundIndex(*val);
      RowMap sec(0, end);
      rm.Intersect(sec);
      return;
    }
    case FilterOp::kLt: {
      uint32_t end = LowerBoundIndex(*val);
      RowMap sec(0, end);
      rm.Intersect(sec);
      return;
    }
    case FilterOp::kGe: {
      uint32_t beg = LowerBoundIndex(*val);
      RowMap sec(beg, size_);
      rm.Intersect(sec);
      return;
    }
    case FilterOp::kGt: {
      uint32_t beg = UpperBoundIndex(*val);
      RowMap sec(beg, size_);
      rm.Intersect(sec);
      return;
    }
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
      rm.Clear();
  }
  return;
}

uint32_t NumericStorage::UpperBoundIndex(NumericValue val,
                                         uint32_t* order) const {
  return std::visit(
      [this, order](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data_);
        auto upper = std::upper_bound(order, order + size_, val_data,
                                      [typed_start](T val, uint32_t index) {
                                        return val < *(typed_start + index);
                                      });
        return static_cast<uint32_t>(std::distance(order, upper));
      },
      val);
}

// As we don't template those functions, we need to use std::visitor to type
// `start`, hence this wrapping.
uint32_t NumericStorage::LowerBoundIndex(NumericValue val,
                                         uint32_t* order) const {
  return std::visit(
      [this, order](auto val_data) {
        using T = decltype(val_data);
        const T* typed_start = static_cast<const T*>(data_);
        auto lower = std::lower_bound(order, order + size_, val_data,
                                      [typed_start](uint32_t index, T val) {
                                        return *(typed_start + index) < val;
                                      });
        return static_cast<uint32_t>(std::distance(order, lower));
      },
      val);
}

void NumericStorage::CompareSortedIndexes(FilterOp op,
                                          SqlValue sql_val,
                                          uint32_t* order,
                                          RowMap& rm) const {
  std::optional<NumericValue> val = GetNumericTypeVariant(type_, sql_val);
  if (!val.has_value() || op == FilterOp::kIsNotNull ||
      op == FilterOp::kIsNull || op == FilterOp::kGlob) {
    rm.Clear();
    return;
  }

  switch (op) {
    case FilterOp::kEq: {
      uint32_t beg = LowerBoundIndex(*val, order);
      uint32_t end = UpperBoundIndex(*val, order);
      std::vector<uint32_t> index(order + beg, order + end);
      rm.Intersect(RowMap(std::move(index)));
      return;
    }
    case FilterOp::kLe: {
      uint32_t end = UpperBoundIndex(*val, order);
      std::vector<uint32_t> index(order, order + end);
      rm.Intersect(RowMap(std::move(index)));
      return;
    }
    case FilterOp::kLt: {
      uint32_t end = LowerBoundIndex(*val, order);
      std::vector<uint32_t> index(order, order + end);
      rm.Intersect(RowMap(std::move(index)));
      return;
    }
    case FilterOp::kGe: {
      uint32_t beg = LowerBoundIndex(*val, order);
      std::vector<uint32_t> index(order + beg, order + size_);
      rm.Intersect(RowMap(std::move(index)));
      return;
    }
    case FilterOp::kGt: {
      uint32_t beg = UpperBoundIndex(*val, order);
      std::vector<uint32_t> index(order + beg, order + size_);
      rm.Intersect(RowMap(std::move(index)));
      return;
    }
    case FilterOp::kNe:
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
    case FilterOp::kGlob:
      rm.Clear();
  }
  return;
}

}  // namespace column
}  // namespace trace_processor
}  // namespace perfetto
