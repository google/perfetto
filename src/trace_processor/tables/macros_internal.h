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
#include "src/trace_processor/db/typed_column.h"

namespace perfetto {
namespace trace_processor {
namespace macros_internal {

// We define this class to allow the table macro below to compile without
// needing templates; in reality none of the methods will be called because the
// pointer to this class will always be null.
class RootParentTable : public Table {
 public:
  struct Row {
   public:
    Row(std::nullptr_t) {}

    const char* type() const { return type_; }

   protected:
    const char* type_ = nullptr;
  };
  uint32_t Insert(const Row&) { PERFETTO_FATAL("Should not be called"); }
};

// The parent class for all macro generated tables.
// This class is used to extract common code from the macro tables to reduce
// code size.
class MacroTable : public Table {
 public:
  MacroTable(const char* name, StringPool* pool, Table* parent)
      : Table(pool, parent), name_(name), parent_(parent) {
    row_maps_.emplace_back(BitVector());
    if (!parent) {
      columns_.emplace_back(
          Column::IdColumn(this, static_cast<uint32_t>(columns_.size()),
                           static_cast<uint32_t>(row_maps_.size()) - 1));
      columns_.emplace_back(
          Column("type", &type_, Column::kNoFlag, this,
                 static_cast<uint32_t>(columns_.size()),
                 static_cast<uint32_t>(row_maps_.size()) - 1));
    }
  }

  const char* table_name() const { return name_; }

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

  // Stores the most specific "derived" type of this row in the table.
  //
  // For example, suppose a row is inserted into the gpu_slice table. This will
  // also cause a row to be inserted into the slice table. For users querying
  // the slice table, they will want to know the "real" type of this slice (i.e.
  // they will want to see that the type is gpu_slice). This sparse vector
  // stores precisely the real type.
  //
  // Only relevant for parentless tables. Will be empty and unreferenced by
  // tables with parents.
  SparseVector<StringPool::Id> type_;

 private:
  const char* name_ = nullptr;
  Table* parent_ = nullptr;
};

}  // namespace macros_internal

// Basic helper macros.
#define PERFETTO_TP_NOOP(...)

// Gets the class name from a table definition.
#define PERFETTO_TP_EXTRACT_TABLE_CLASS(class_name, ...) class_name
#define PERFETTO_TP_TABLE_CLASS(DEF) \
  DEF(PERFETTO_TP_EXTRACT_TABLE_CLASS, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP)

// Gets the table name from the table definition.
#define PERFETTO_TP_EXTRACT_TABLE_NAME(_, table_name) table_name
#define PERFETTO_TP_TABLE_NAME(DEF) \
  DEF(PERFETTO_TP_EXTRACT_TABLE_NAME, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP)

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
#define PERFETTO_TP_NAME_COMMA(type, name, ...) name,
#define PERFETTO_TP_TYPE_NAME_COMMA(type, name, ...) type name,

// Constructor parameters of Table::Row.
// We name this name_c to avoid a clash with the field names of
// Table::Row.
#define PERFETTO_TP_ROW_CONSTRUCTOR(type, name, ...) type name##_c = {},

// Constructor parameters for parent of Row.
#define PERFETTO_TP_PARENT_ROW_CONSTRUCTOR(type, name, ...) name##_c,

// Initializes the members of Table::Row.
#define PERFETTO_TP_ROW_INITIALIZER(type, name, ...) name = name##_c;

// Defines the variable in Table::Row.
#define PERFETTO_TP_ROW_DEFINITION(type, name, ...) type name = {};

// Defines the parent row field in Insert.
#define PERFETTO_TP_PARENT_ROW_INSERT(type, name, ...) row.name,

// Defines the member variable in the Table.
#define PERFETTO_TP_TABLE_MEMBER(type, name, ...) \
  SparseVector<TypedColumn<type>::StoredType> name##_;

// Constructs the column in the Table constructor when flags are specified.
#define PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN_FLAGS(type, name, flags)        \
  columns_.emplace_back(#name, &name##_, static_cast<uint32_t>(flags), this, \
                        columns_.size(), row_maps_.size() - 1);

// Constructs the column in the Table constructor when no flags are specified.
#define PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN_NO_FLAGS(type, name) \
  columns_.emplace_back(#name, &name##_, Column::kNoFlag, this,   \
                        columns_.size(), row_maps_.size() - 1);

// Chooses between the flag and no-flag variant based on the whether there
// are two or three arguments.
#define PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN_CHOOSER(type, name, maybe_flags, \
                                                     fn, ...)                 \
  fn

// Invokes the chosen column constructor by passing the given args.
#define PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN(...)              \
  PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN_CHOOSER(                \
      __VA_ARGS__, PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN_FLAGS, \
      PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN_NO_FLAGS)           \
  (__VA_ARGS__)

// Inserts the value into the corresponding column
#define PERFETTO_TP_COLUMN_APPEND(type, name, ...) \
  mutable_##name()->Append(std::move(row.name));

// Defines the accessors for a column.
#define PERFETTO_TP_TABLE_COL_ACCESSOR(type, name, ...)       \
  const TypedColumn<type>& name() const {                     \
    return static_cast<const TypedColumn<type>&>(             \
        columns_[static_cast<uint32_t>(ColumnIndex::name)]);  \
  }                                                           \
                                                              \
  TypedColumn<type>* mutable_##name() {                       \
    return static_cast<TypedColumn<type>*>(                   \
        &columns_[static_cast<uint32_t>(ColumnIndex::name)]); \
  }

// Definition used as the parent of root tables.
#define PERFETTO_TP_ROOT_TABLE_PARENT_DEF(NAME, PARENT, C) \
  NAME(macros_internal::RootParentTable, "root")

// For more general documentation, see PERFETTO_TP_TABLE in macros.h.
#define PERFETTO_TP_TABLE_INTERNAL(table_name, class_name, parent_class_name, \
                                   DEF)                                       \
  class class_name : public macros_internal::MacroTable {                     \
   public:                                                                    \
    struct Row : parent_class_name::Row {                                     \
      /*                                                                      \
       * Expands to Row(col_type1 col1_c, base::Optional<col_type2> col2_c,   \
       * ...)                                                                 \
       */                                                                     \
      Row(PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_ROW_CONSTRUCTOR)           \
              std::nullptr_t = nullptr)                                       \
          : parent_class_name::Row(PERFETTO_TP_PARENT_COLUMNS(                \
                DEF,                                                          \
                PERFETTO_TP_PARENT_ROW_CONSTRUCTOR) nullptr) {                \
        type_ = table_name;                                                   \
                                                                              \
        /* Expands to                                                         \
         * col1 = col1_c;                                                     \
         * col2 = col2_c;                                                     \
         * ...                                                                \
         */                                                                   \
        PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_ROW_INITIALIZER)           \
      }                                                                       \
                                                                              \
      /* Expands to                                                           \
       * col_type1 col1 = {};                                                 \
       * base::Optional<col_type2> col2 = {};                                 \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_ROW_DEFINITION)              \
    };                                                                        \
                                                                              \
    class_name(StringPool* pool, parent_class_name* parent)                   \
        : macros_internal::MacroTable(table_name, pool, parent),              \
          parent_(parent) {                                                   \
      /* Expands to                                                           \
       * columns_.emplace_back("col1", col1_, Column::kNoFlag, this,          \
       *                        columns_.size(), row_maps_.size() - 1);       \
       * columns_.emplace_back("col2", col2_, Column::kNoFlag, this,          \
       *                       columns_.size(), row_maps_.size() - 1);        \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN);   \
    }                                                                         \
                                                                              \
    uint32_t Insert(const Row& row) {                                         \
      uint32_t id;                                                            \
      if (parent_ == nullptr) {                                               \
        id = size();                                                          \
        type_.Append(string_pool_->InternString(row.type()));                 \
      } else {                                                                \
        id = parent_->Insert(row);                                            \
      }                                                                       \
      UpdateRowMapsAfterParentInsert();                                       \
                                                                              \
      /* Expands to                                                           \
       * col1_.Append(row.col1);                                              \
       * col2_.Append(row.col2);                                              \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_COLUMN_APPEND);              \
      return id;                                                              \
    }                                                                         \
                                                                              \
    const Column& id() const {                                                \
      return columns_[static_cast<uint32_t>(ColumnIndex::id)];                \
    }                                                                         \
                                                                              \
    const TypedColumn<StringPool::Id>& type() const {                         \
      return static_cast<const TypedColumn<StringPool::Id>&>(                 \
          columns_[static_cast<uint32_t>(ColumnIndex::type)]);                \
    }                                                                         \
                                                                              \
    /* Expands to                                                             \
     * const TypedColumn<col1_type>& col1() { return col1_; }                 \
     * TypedColumn<col1_type>* mutable_col1() { return &col1_; }              \
     * const TypedColumn<col2_type>& col2() { return col2_; }                 \
     * TypedColumn<col2_type>* mutable_col2() { return &col2_; }              \
     * ...                                                                    \
     */                                                                       \
    PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_COL_ACCESSOR)              \
                                                                              \
   private:                                                                   \
    enum class ColumnIndex : uint32_t {                                       \
      id,                                                                     \
      type, /* Expands to col1, col2, ... */                                  \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_NAME_COMMA) kNumCols           \
    };                                                                        \
                                                                              \
    parent_class_name* parent_;                                               \
                                                                              \
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
