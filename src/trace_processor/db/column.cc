/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/db/column.h"

#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {

namespace {

// This code mathces the behaviour of sqlite3IntFloatCompare to ensure that
// we are consistent with SQLite.
int CompareIntToDouble(int64_t i, double d) {
  // First check if we are out of range for a int64_t. We use the constants
  // directly instead of using numeric_limits as the casts introduces rounding
  // in the doubles as a double cannot exactly represent int64::max().
  if (d >= 9223372036854775808.0)
    return -1;
  if (d < -9223372036854775808.0)
    return 1;

  // Then, try to compare in int64 space to try and keep as much precision as
  // possible.
  int64_t d_i = static_cast<int64_t>(d);
  if (i < d_i)
    return -1;
  if (i > d_i)
    return 1;

  // Finally, try and compare in double space, sacrificing precision if
  // necessary.
  double i_d = static_cast<double>(i);
  return (i_d < d) ? -1 : (i_d > d ? 1 : 0);
}

}  // namespace

Column::Column(const Column& column,
               Table* table,
               uint32_t col_idx,
               uint32_t row_map_idx)
    : Column(column.name_,
             column.type_,
             column.flags_,
             table,
             col_idx,
             row_map_idx,
             column.sparse_vector_) {}

Column::Column(const char* name,
               ColumnType type,
               uint32_t flags,
               Table* table,
               uint32_t col_idx_in_table,
               uint32_t row_map_idx,
               void* sparse_vector)
    : type_(type),
      sparse_vector_(sparse_vector),
      name_(name),
      flags_(flags),
      table_(table),
      col_idx_in_table_(col_idx_in_table),
      row_map_idx_(row_map_idx),
      string_pool_(table->string_pool_) {}

Column Column::IdColumn(Table* table, uint32_t col_idx, uint32_t row_map_idx) {
  return Column("id", ColumnType::kId, Flag::kSorted | Flag::kNonNull, table,
                col_idx, row_map_idx, nullptr);
}

void Column::StableSort(bool desc, std::vector<uint32_t>* idx) const {
  if (desc) {
    StableSort<true /* desc */>(idx);
  } else {
    StableSort<false /* desc */>(idx);
  }
}

void Column::FilterIntoSlow(FilterOp op, SqlValue value, RowMap* rm) const {
  switch (type_) {
    case ColumnType::kInt32: {
      if (IsNullable()) {
        FilterIntoNumericSlow<int32_t, true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoNumericSlow<int32_t, false /* is_nullable */>(op, value, rm);
      }
      break;
    }
    case ColumnType::kUint32: {
      if (IsNullable()) {
        FilterIntoNumericSlow<uint32_t, true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoNumericSlow<uint32_t, false /* is_nullable */>(op, value, rm);
      }
      break;
    }
    case ColumnType::kInt64: {
      if (IsNullable()) {
        FilterIntoNumericSlow<int64_t, true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoNumericSlow<int64_t, false /* is_nullable */>(op, value, rm);
      }
      break;
    }
    case ColumnType::kDouble: {
      if (IsNullable()) {
        FilterIntoNumericSlow<double, true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoNumericSlow<double, false /* is_nullable */>(op, value, rm);
      }
      break;
    }
    case ColumnType::kString: {
      FilterIntoStringSlow(op, value, rm);
      break;
    }
    case ColumnType::kId: {
      FilterIntoIdSlow(op, value, rm);
      break;
    }
  }
}

template <typename T, bool is_nullable>
void Column::FilterIntoNumericSlow(FilterOp op,
                                   SqlValue value,
                                   RowMap* rm) const {
  PERFETTO_DCHECK(IsNullable() == is_nullable);
  PERFETTO_DCHECK(type_ == ToColumnType<T>());
  PERFETTO_DCHECK(std::is_arithmetic<T>::value);

  if (op == FilterOp::kIsNull) {
    PERFETTO_DCHECK(value.is_null());
    if (is_nullable) {
      row_map().FilterInto(rm, [this](uint32_t row) {
        return !sparse_vector<T>().Get(row).has_value();
      });
    } else {
      rm->Intersect(RowMap());
    }
    return;
  } else if (op == FilterOp::kIsNotNull) {
    PERFETTO_DCHECK(value.is_null());
    if (is_nullable) {
      row_map().FilterInto(rm, [this](uint32_t row) {
        return sparse_vector<T>().Get(row).has_value();
      });
    }
    return;
  }

  if (value.type == SqlValue::Type::kDouble) {
    double double_value = value.double_value;
    if (std::is_same<T, double>::value) {
      auto fn = [double_value](T v) {
        return v < double_value ? -1 : (v > double_value ? 1 : 0);
      };
      FilterIntoNumericWithComparatorSlow<T, is_nullable>(op, rm, fn);
    } else {
      auto fn = [double_value](T v) {
        // We static cast here as this code will be compiled even when T ==
        // double as we don't have if constexpr in C++11. In reality the cast is
        // a noop but we cannot statically verify that for the compiler.
        return CompareIntToDouble(static_cast<int64_t>(v), double_value);
      };
      FilterIntoNumericWithComparatorSlow<T, is_nullable>(op, rm, fn);
    }
  } else if (value.type == SqlValue::Type::kLong) {
    int64_t long_value = value.long_value;
    if (std::is_same<T, double>::value) {
      auto fn = [long_value](T v) {
        // We negate the return value as the long is always the first parameter
        // for this function even though the LHS of the comparator should
        // actually be |v|. This saves us having a duplicate implementation of
        // the comparision function.
        return -CompareIntToDouble(long_value, v);
      };
      FilterIntoNumericWithComparatorSlow<T, is_nullable>(op, rm, fn);
    } else {
      auto fn = [long_value](T v) {
        return v < long_value ? -1 : (v > long_value ? 1 : 0);
      };
      FilterIntoNumericWithComparatorSlow<T, is_nullable>(op, rm, fn);
    }
  } else {
    rm->Intersect(RowMap());
  }
}

template <typename T, bool is_nullable, typename Comparator>
void Column::FilterIntoNumericWithComparatorSlow(FilterOp op,
                                                 RowMap* rm,
                                                 Comparator cmp) const {
  switch (op) {
    case FilterOp::kLt:
      row_map().FilterInto(rm, [this, &cmp](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && cmp(*opt_value) < 0;
        }
        return cmp(sparse_vector<T>().GetNonNull(idx)) < 0;
      });
      break;
    case FilterOp::kEq:
      row_map().FilterInto(rm, [this, &cmp](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && cmp(*opt_value) == 0;
        }
        return cmp(sparse_vector<T>().GetNonNull(idx)) == 0;
      });
      break;
    case FilterOp::kGt:
      row_map().FilterInto(rm, [this, &cmp](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && cmp(*opt_value) > 0;
        }
        return cmp(sparse_vector<T>().GetNonNull(idx)) > 0;
      });
      break;
    case FilterOp::kNe:
      row_map().FilterInto(rm, [this, &cmp](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && cmp(*opt_value) != 0;
        }
        return cmp(sparse_vector<T>().GetNonNull(idx)) != 0;
      });
      break;
    case FilterOp::kLe:
      row_map().FilterInto(rm, [this, &cmp](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && cmp(*opt_value) <= 0;
        }
        return cmp(sparse_vector<T>().GetNonNull(idx)) <= 0;
      });
      break;
    case FilterOp::kGe:
      row_map().FilterInto(rm, [this, &cmp](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && cmp(*opt_value) >= 0;
        }
        return cmp(sparse_vector<T>().GetNonNull(idx)) >= 0;
      });
      break;
    case FilterOp::kLike:
      rm->Intersect(RowMap());
      break;
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
      PERFETTO_FATAL("Should be handled above");
  }
}

void Column::FilterIntoStringSlow(FilterOp op,
                                  SqlValue value,
                                  RowMap* rm) const {
  PERFETTO_DCHECK(type_ == ColumnType::kString);

  if (op == FilterOp::kIsNull) {
    PERFETTO_DCHECK(value.is_null());
    row_map().FilterInto(rm, [this](uint32_t row) {
      return GetStringPoolStringAtIdx(row).data() == nullptr;
    });
    return;
  } else if (op == FilterOp::kIsNotNull) {
    PERFETTO_DCHECK(value.is_null());
    row_map().FilterInto(rm, [this](uint32_t row) {
      return GetStringPoolStringAtIdx(row).data() != nullptr;
    });
    return;
  }

  if (value.type != SqlValue::Type::kString) {
    rm->Intersect(RowMap());
    return;
  }

  NullTermStringView str_value = value.string_value;
  switch (op) {
    case FilterOp::kLt:
      row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
        auto v = GetStringPoolStringAtIdx(idx);
        return v.data() != nullptr && v < str_value;
      });
      break;
    case FilterOp::kEq:
      row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
        auto v = GetStringPoolStringAtIdx(idx);
        return v.data() != nullptr && v == str_value;
      });
      break;
    case FilterOp::kGt:
      row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
        auto v = GetStringPoolStringAtIdx(idx);
        return v.data() != nullptr && v > str_value;
      });
      break;
    case FilterOp::kNe:
      row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
        auto v = GetStringPoolStringAtIdx(idx);
        return v.data() != nullptr && v != str_value;
      });
      break;
    case FilterOp::kLe:
      row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
        auto v = GetStringPoolStringAtIdx(idx);
        return v.data() != nullptr && v <= str_value;
      });
      break;
    case FilterOp::kGe:
      row_map().FilterInto(rm, [this, str_value](uint32_t idx) {
        auto v = GetStringPoolStringAtIdx(idx);
        return v.data() != nullptr && v >= str_value;
      });
      break;
    case FilterOp::kLike:
      // TODO(lalitm): either call through to SQLite or reimplement
      // like ourselves.
      PERFETTO_DLOG("Ignoring like constraint on string column");
      break;
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
      PERFETTO_FATAL("Should be handled above");
  }
}

void Column::FilterIntoIdSlow(FilterOp op, SqlValue value, RowMap* rm) const {
  PERFETTO_DCHECK(type_ == ColumnType::kId);

  if (op == FilterOp::kIsNull) {
    PERFETTO_DCHECK(value.is_null());
    rm->Intersect(RowMap());
    return;
  } else if (op == FilterOp::kIsNotNull) {
    PERFETTO_DCHECK(value.is_null());
    return;
  }

  if (value.type != SqlValue::Type::kLong) {
    rm->Intersect(RowMap());
    return;
  }

  uint32_t id_value = static_cast<uint32_t>(value.long_value);
  switch (op) {
    case FilterOp::kLt:
      row_map().FilterInto(rm,
                           [id_value](uint32_t idx) { return idx < id_value; });
      break;
    case FilterOp::kEq:
      row_map().FilterInto(
          rm, [id_value](uint32_t idx) { return idx == id_value; });
      break;
    case FilterOp::kGt:
      row_map().FilterInto(rm,
                           [id_value](uint32_t idx) { return idx > id_value; });
      break;
    case FilterOp::kNe:
      row_map().FilterInto(
          rm, [id_value](uint32_t idx) { return idx != id_value; });
      break;
    case FilterOp::kLe:
      row_map().FilterInto(
          rm, [id_value](uint32_t idx) { return idx <= id_value; });
      break;
    case FilterOp::kGe:
      row_map().FilterInto(
          rm, [id_value](uint32_t idx) { return idx >= id_value; });
      break;
    case FilterOp::kLike:
      rm->Intersect(RowMap());
      break;
    case FilterOp::kIsNull:
    case FilterOp::kIsNotNull:
      PERFETTO_FATAL("Should be handled above");
  }
}

template <bool desc>
void Column::StableSort(std::vector<uint32_t>* out) const {
  switch (type_) {
    case ColumnType::kInt32: {
      if (IsNullable()) {
        StableSort<desc, int32_t, true /* is_nullable */>(out);
      } else {
        StableSort<desc, int32_t, false /* is_nullable */>(out);
      }
      break;
    }
    case ColumnType::kUint32: {
      if (IsNullable()) {
        StableSort<desc, uint32_t, true /* is_nullable */>(out);
      } else {
        StableSort<desc, uint32_t, false /* is_nullable */>(out);
      }
      break;
    }
    case ColumnType::kInt64: {
      if (IsNullable()) {
        StableSort<desc, int64_t, true /* is_nullable */>(out);
      } else {
        StableSort<desc, int64_t, false /* is_nullable */>(out);
      }
      break;
    }
    case ColumnType::kDouble: {
      if (IsNullable()) {
        StableSort<desc, double, true /* is_nullable */>(out);
      } else {
        StableSort<desc, double, false /* is_nullable */>(out);
      }
      break;
    }
    case ColumnType::kString: {
      row_map().StableSort(out, [this](uint32_t a_idx, uint32_t b_idx) {
        auto a_str = GetStringPoolStringAtIdx(a_idx);
        auto b_str = GetStringPoolStringAtIdx(b_idx);
        return desc ? b_str < a_str : a_str < b_str;
      });
      break;
    }
    case ColumnType::kId:
      row_map().StableSort(out, [](uint32_t a_idx, uint32_t b_idx) {
        return desc ? b_idx < a_idx : a_idx < b_idx;
      });
  }
}

template <bool desc, typename T, bool is_nullable>
void Column::StableSort(std::vector<uint32_t>* out) const {
  PERFETTO_DCHECK(IsNullable() == is_nullable);
  PERFETTO_DCHECK(ToColumnType<T>() == type_);

  const auto& sv = sparse_vector<T>();
  row_map().StableSort(out, [&sv](uint32_t a_idx, uint32_t b_idx) {
    if (is_nullable) {
      auto a_val = sv.Get(a_idx);
      auto b_val = sv.Get(b_idx);
      return desc ? b_val < a_val : a_val < b_val;
    }
    auto a_val = sv.GetNonNull(a_idx);
    auto b_val = sv.GetNonNull(b_idx);
    return desc ? b_val < a_val : a_val < b_val;
  });
}

const RowMap& Column::row_map() const {
  return table_->row_maps_[row_map_idx_];
}

}  // namespace trace_processor
}  // namespace perfetto
