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

#ifndef SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_
#define SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_

#include <type_traits>

#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {
namespace macros_internal {

// We define this class to allow the table macro below to compile without
// needing templates; in reality none of the methods will be called because the
// pointer to this class will always be null.
class RootParentTable : public Table {
 public:
  uint32_t Insert(std::nullptr_t) { PERFETTO_FATAL("Should not be called"); }
};

// The parent class for all macro generated tables.
// This class is used to extract common code from the macro tables to reduce
// code size.
class MacroTable : public Table {
 public:
  MacroTable(Table* parent) : Table(parent), parent_(parent) {
    if (!parent) {
      columns_.emplace_back(
          Column::IdColumn(this, static_cast<uint32_t>(columns_.size()),
                           static_cast<uint32_t>(row_maps_.size()) - 1));
    }
    row_maps_.emplace_back(BitVector());
  }

 protected:
  void UpdateRowMapsAfterParentInsert() {
    if (parent_ != nullptr) {
      // If there is a parent table, add the last inserted row in each of the
      // parent row maps to the corresponding row map in the child.
      for (uint32_t i = 0; i < parent_->row_maps().size(); ++i) {
        const RowMap& parent_rm = parent_->row_maps()[i];
        row_maps_[i].Add(parent_rm.Get(parent_rm.size() - 1));
      }
    }
    // Also add the index of the new row to the identity row map and increment
    // the size.
    row_maps_.back().Add(size_++);
  }

 private:
  Table* parent_ = nullptr;
};

}  // namespace macros_internal

// Basic helper macros.
#define PERFETTO_TP_NOOP(...)

// Gets the class name from a table definition.
#define PERFETTO_TP_EXTRACT_TABLE_CLASS(class_name) class_name
#define PERFETTO_TP_TABLE_CLASS(DEF) \
  DEF(PERFETTO_TP_EXTRACT_TABLE_CLASS, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP)

// Gets the parent definition from a table definition.
#define PERFETTO_TP_EXTRACT_PARENT_DEF(PARENT_DEF, _) PARENT_DEF
#define PERFETTO_TP_PARENT_DEF(DEF) \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_EXTRACT_PARENT_DEF, PERFETTO_TP_NOOP)

// Invokes FN on each column in the definition of the table. We define a
// recursive macro as we need to walk up the hierarchy until we hit the root.
// Currently, we hardcode 5 levels but this can be increased as necessary.
#define PERFETTO_TP_ALL_COLUMNS_0(DEF, arg) \
  static_assert(false, "Macro recursion depth exceeded");
#define PERFETTO_TP_ALL_COLUMNS_1(DEF, arg) \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_ALL_COLUMNS_0, arg)
#define PERFETTO_TP_ALL_COLUMNS_2(DEF, arg) \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_ALL_COLUMNS_1, arg)
#define PERFETTO_TP_ALL_COLUMNS_3(DEF, arg) \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_ALL_COLUMNS_2, arg)
#define PERFETTO_TP_ALL_COLUMNS_4(DEF, arg) \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_ALL_COLUMNS_3, arg)
#define PERFETTO_TP_ALL_COLUMNS(DEF, arg) \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_ALL_COLUMNS_4, arg)

// Invokes FN on each column in the table definition.
#define PERFETTO_TP_TABLE_COLUMNS(DEF, FN) \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, FN)

// Invokes FN on each column in every ancestor of the table.
#define PERFETTO_TP_PARENT_COLUMNS(DEF, FN) \
  PERFETTO_TP_ALL_COLUMNS(PERFETTO_TP_PARENT_DEF(DEF), FN)

// Basic macros for extracting column info from a schema.
#define PERFETTO_TP_TYPE_COMMA(type, name) type,
#define PERFETTO_TP_NAME_COMMA(type, name) name,
#define PERFETTO_TP_TYPE_NAME_COMMA(type, name) type name,

// Defines the member variable in the Table.
#define PERFETTO_TP_TABLE_MEMBER(type, name) SparseVector<type> name##_;

// Constructs the column in the Table constructor.
#define PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN(type, name)        \
  columns_.emplace_back(#name, &name##_, this, columns_.size(), \
                        row_maps_.size() - 1);

// Inserts the a value into the corresponding column
#define PERFETTO_TP_COLUMN_APPEND(type, name) name##_.Append(name);

// Defines the accessor for a column.
#define PERFETTO_TP_TABLE_COL_ACCESSOR(type, name)             \
  const Column& name() const {                                 \
    return columns_[static_cast<uint32_t>(ColumnIndex::name)]; \
  }

// For more general documentation, see PERFETTO_TP_TABLE in macros.h.
#define PERFETTO_TP_TABLE_INTERNAL(class_name, parent_class_name, DEF)        \
  class class_name : public macros_internal::MacroTable {                     \
   public:                                                                    \
    class_name(parent_class_name* parent)                                     \
        : macros_internal::MacroTable(parent), parent_(parent) {              \
      /* Expands to                                                           \
       * columns_.emplace_back("col1", col1_, this, columns_.size(),          \
       *                       row_maps_.size() - 1);                         \
       * columns_.emplace_back("col2", col2_, this, columns_.size(),          \
       *                       row_maps_.size() - 1);                         \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN);   \
    }                                                                         \
                                                                              \
    /* Expands to Insert(col_type1 col1, col_type2 col2, ...) */              \
    uint32_t Insert(PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TYPE_NAME_COMMA) \
                        std::nullptr_t = nullptr) {                           \
      uint32_t id;                                                            \
      if (parent_ == nullptr) {                                               \
        id = size();                                                          \
      } else {                                                                \
        /* Expands to parent_->Insert(parent_col_1, parent_col_2, ...) */     \
        id = parent_->Insert(                                                 \
            PERFETTO_TP_PARENT_COLUMNS(DEF, PERFETTO_TP_NAME_COMMA) nullptr); \
      }                                                                       \
      UpdateRowMapsAfterParentInsert();                                       \
                                                                              \
      /* Expands to                                                           \
       * col1_.Append(col1);                                                  \
       * col2_.Append(col2);                                                  \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_COLUMN_APPEND);              \
      return id;                                                              \
    }                                                                         \
                                                                              \
    /* Expands to                                                             \
     * const SparseVector<col1_type>& col1() { return col1_; }                \
     * const SparseVector<col2_type>& col2() { return col2_; }                \
     * ...                                                                    \
     */                                                                       \
    PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_COL_ACCESSOR)              \
                                                                              \
   private:                                                                   \
    enum class ColumnIndex : uint32_t {                                       \
      id, /* Expands to col1, col2, ... */                                    \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_NAME_COMMA) kNumCols           \
    };                                                                        \
                                                                              \
    parent_class_name* parent_;                                               \
    /* Expands to                                                             \
     * SparseVector<col1_type> col1_;                                         \
     * SparseVector<col2_type> col2_;                                         \
     * ...                                                                    \
     */                                                                       \
    PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_MEMBER)                  \
  }

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_
