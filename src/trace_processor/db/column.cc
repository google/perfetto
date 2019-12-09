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
        FilterIntoLongSlow<int32_t, true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoLongSlow<int32_t, false /* is_nullable */>(op, value, rm);
      }
      break;
    }
    case ColumnType::kUint32: {
      if (IsNullable()) {
        FilterIntoLongSlow<uint32_t, true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoLongSlow<uint32_t, false /* is_nullable */>(op, value, rm);
      }
      break;
    }
    case ColumnType::kInt64: {
      if (IsNullable()) {
        FilterIntoLongSlow<int64_t, true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoLongSlow<int64_t, false /* is_nullable */>(op, value, rm);
      }
      break;
    }
    case ColumnType::kDouble: {
      if (IsNullable()) {
        FilterIntoDoubleSlow<true /* is_nullable */>(op, value, rm);
      } else {
        FilterIntoDoubleSlow<false /* is_nullable */>(op, value, rm);
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
void Column::FilterIntoLongSlow(FilterOp op, SqlValue value, RowMap* rm) const {
  PERFETTO_DCHECK(IsNullable() == is_nullable);
  PERFETTO_DCHECK(type_ == ToColumnType<T>());

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

  if (value.type != SqlValue::Type::kLong) {
    rm->Intersect(RowMap());
    return;
  }

  int64_t long_value = value.long_value;
  switch (op) {
    case FilterOp::kLt:
      row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && *opt_value < long_value;
        }
        return sparse_vector<T>().GetNonNull(idx) < long_value;
      });
      break;
    case FilterOp::kEq:
      row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && opt_value == long_value;
        }
        return sparse_vector<T>().GetNonNull(idx) == long_value;
      });
      break;
    case FilterOp::kGt:
      row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && opt_value > long_value;
        }
        return sparse_vector<T>().GetNonNull(idx) > long_value;
      });
      break;
    case FilterOp::kNe:
      row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && opt_value != long_value;
        }
        return sparse_vector<T>().GetNonNull(idx) != long_value;
      });
      break;
    case FilterOp::kLe:
      row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && opt_value <= long_value;
        }
        return sparse_vector<T>().GetNonNull(idx) <= long_value;
      });
      break;
    case FilterOp::kGe:
      row_map().FilterInto(rm, [this, long_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<T>().Get(idx);
          return opt_value && opt_value >= long_value;
        }
        return sparse_vector<T>().GetNonNull(idx) >= long_value;
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

template <bool is_nullable>
void Column::FilterIntoDoubleSlow(FilterOp op,
                                  SqlValue value,
                                  RowMap* rm) const {
  PERFETTO_DCHECK(IsNullable() == is_nullable);
  PERFETTO_DCHECK(type_ == ColumnType::kDouble);

  if (op == FilterOp::kIsNull) {
    PERFETTO_DCHECK(value.is_null());
    if (is_nullable) {
      row_map().FilterInto(rm, [this](uint32_t row) {
        return !sparse_vector<double>().Get(row).has_value();
      });
    } else {
      rm->Intersect(RowMap());
    }
    return;
  } else if (op == FilterOp::kIsNotNull) {
    PERFETTO_DCHECK(value.is_null());
    if (is_nullable) {
      row_map().FilterInto(rm, [this](uint32_t row) {
        return sparse_vector<double>().Get(row).has_value();
      });
    }
    return;
  }

  if (value.type != SqlValue::Type::kDouble) {
    rm->Intersect(RowMap());
    return;
  }

  double double_value = value.double_value;
  switch (op) {
    case FilterOp::kLt:
      row_map().FilterInto(rm, [this, double_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<double>().Get(idx);
          return opt_value && opt_value < double_value;
        }
        return sparse_vector<double>().GetNonNull(idx) < double_value;
      });
      break;
    case FilterOp::kEq:
      row_map().FilterInto(rm, [this, double_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<double>().Get(idx);
          return opt_value && std::equal_to<double>()(*opt_value, double_value);
        }
        auto v = sparse_vector<double>().GetNonNull(idx);
        return std::equal_to<double>()(v, double_value);
      });
      break;
    case FilterOp::kGt:
      row_map().FilterInto(rm, [this, double_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<double>().Get(idx);
          return opt_value && opt_value > double_value;
        }
        return sparse_vector<double>().GetNonNull(idx) > double_value;
      });
      break;
    case FilterOp::kNe:
      row_map().FilterInto(rm, [this, double_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<double>().Get(idx);
          return opt_value &&
                 std::not_equal_to<double>()(*opt_value, double_value);
        }
        auto v = sparse_vector<double>().GetNonNull(idx);
        return std::not_equal_to<double>()(v, double_value);
      });
      break;
    case FilterOp::kLe:
      row_map().FilterInto(rm, [this, double_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<double>().Get(idx);
          return opt_value && opt_value <= double_value;
        }
        return sparse_vector<double>().GetNonNull(idx) <= double_value;
      });
      break;
    case FilterOp::kGe:
      row_map().FilterInto(rm, [this, double_value](uint32_t idx) {
        if (is_nullable) {
          auto opt_value = sparse_vector<double>().Get(idx);
          return opt_value && opt_value >= double_value;
        }
        return sparse_vector<double>().GetNonNull(idx) >= double_value;
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
