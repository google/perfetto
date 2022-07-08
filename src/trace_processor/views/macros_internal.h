/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include "src/trace_processor/db/view.h"

#include "src/trace_processor/tables/macros_internal.h"

#ifndef SRC_TRACE_PROCESSOR_VIEWS_MACROS_INTERNAL_H_
#define SRC_TRACE_PROCESSOR_VIEWS_MACROS_INTERNAL_H_

namespace perfetto {
namespace trace_processor {
namespace macros_internal {

template <typename T>
class ViewColumnBlueprint {
 public:
  using sql_value_type = typename TypedColumn<T>::sql_value_type;

  explicit ViewColumnBlueprint(uint32_t index_in_view)
      : index_in_view_(index_in_view) {}

  // Returns an Order for each Order type for this ColumnBlueprint.
  Constraint eq(sql_value_type v) const {
    return Constraint{index_in_view_, FilterOp::kEq, ToValue(v)};
  }
  Constraint gt(sql_value_type v) const {
    return Constraint{index_in_view_, FilterOp::kGt, ToValue(v)};
  }
  Constraint lt(sql_value_type v) const {
    return Constraint{index_in_view_, FilterOp::kLt, ToValue(v)};
  }
  Constraint ne(sql_value_type v) const {
    return Constraint{index_in_view_, FilterOp::kNe, ToValue(v)};
  }
  Constraint ge(sql_value_type v) const {
    return Constraint{index_in_view_, FilterOp::kGe, ToValue(v)};
  }
  Constraint le(sql_value_type v) const {
    return Constraint{index_in_view_, FilterOp::kLe, ToValue(v)};
  }

  // Returns an Order for each Order type for this ColumnBlueprint.
  Order ascending() const { return Order{index_in_view_, false}; }
  Order descending() const { return Order{index_in_view_, true}; }

 private:
  static SqlValue ToValue(double value) { return SqlValue::Double(value); }
  static SqlValue ToValue(uint32_t value) { return SqlValue::Long(value); }
  static SqlValue ToValue(int64_t value) { return SqlValue::Long(value); }
  static SqlValue ToValue(NullTermStringView value) {
    return SqlValue::String(value.c_str());
  }

  uint32_t index_in_view_ = 0;
};

// Ignore GCC warning about a missing argument for a variadic macro parameter.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif

// Invokes a View column function using data from a table column definition.
#define PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(FN, col_name) \
  FN(col_name, from_table, col_name)

// Invokes FN on the name and class name declaration of the view.
#define PERFETTO_TP_VIEW_NAME(DEF, FN)                          \
  DEF(FN, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, \
      PERFETTO_TP_NOOP)

// Invokes FN on every table which is part of the view definition.
#define PERFETTO_TP_VIEW_FROM(DEF, FN)                          \
  DEF(PERFETTO_TP_NOOP, FN, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, \
      PERFETTO_TP_NOOP)

// Invokes FN on every join which is part of the view definition.
#define PERFETTO_TP_VIEW_JOINS(DEF, FN)                         \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, FN, PERFETTO_TP_NOOP, \
      PERFETTO_TP_NOOP)

// Invokes FN on every column which is part of the view definition.
#define PERFETTO_TP_VIEW_COLUMNS(DEF, FROM_FROM_COLUMN_FN, FN)                \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, \
      FROM_FROM_COLUMN_FN)                                                    \
  DEF(PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, PERFETTO_TP_NOOP, FN,               \
      PERFETTO_TP_NOOP)

// Gets the class name from a view definition.
#define PERFETTO_TP_VIEW_CLASS_EXTRACT(class_name, ...) class_name

// Gets the view name from the view definition.
#define PERFETTO_TP_VIEW_NAME_EXTRACT(_, view_name) view_name

// Defines a table pointer for the macro constructor.
#define PERFETTO_TP_VIEW_CLASS_TABLE_COMMA_FROM(class_name, table_name, ...) \
  class_name *table_name,

// Defines a table pointer for the macro constructor.
#define PERFETTO_TP_VIEW_CLASS_TABLE_COMMA_JOIN(class_name, table_name, ...) \
  class_name *table_name,

// Defines a View table for the FROM table.
#define PERFETTO_TP_VIEW_FROM_PTR_NAME(_, table_name, ...) \
  table_name, #table_name

// Defines a View::Join struct for a join defined in the macro.
#define PERFETTO_TP_VIEW_JOIN_DEFN(_, join_table, join_col, prev_table, \
                                   prev_col, flags)                     \
  View::JoinTable{join_table,  #join_table, #join_col,                  \
                  #prev_table, #prev_col,   flags},

// Defines a View::Column struct for a column defined in the macro.
#define PERFETTO_TP_VIEW_COLUMN_DEFN(col_name, source_table, source_col) \
  View::OutputColumn{#col_name, TableName::source_table(), #source_col},

// Defines a View::Column struct for a column defined in the FROM table.
#define PERFETTO_TP_VIEW_FROM_COLUMN_DEFN(_, col_name, ...)                    \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(PERFETTO_TP_VIEW_COLUMN_DEFN, \
                                                 col_name)

// Define a TableType alias for the FROM table.
#define PERFETTO_TP_VIEW_TABLE_TYPE(class_name, table_name, ...) \
  using table_name = class_name;

// Define a special "from_table" TableType alias for the "FROM" table.
#define PERFETTO_TP_VIEW_TABLE_TYPE_FROM_ALIAS(_, table_name, ...) \
  using from_table = table_name;

// Defines the table name for each table in the view.
#define PERFETTO_TP_VIEW_TABLE_NAME(_, table_name, ...) \
  static constexpr const char* table_name() { return #table_name; }

// Define a special "from_table" TableType alias for the "FROM" table.
#define PERFETTO_TP_VIEW_TABLE_NAME_FROM_ALIAS(_, table_name, ...) \
  static constexpr const char* from_table() { return #table_name; }

// Define a ColumnType alias for each column.
#define PERFETTO_TP_VIEW_COLUMN_TYPE(col_name, source_table, source_col) \
  using col_name = typename TableType::source_table::ColumnType::source_col;

// Define a ColumnType alias for each column in the FROM table.
#define PERFETTO_TP_VIEW_FROM_COLUMN_TYPE(_, col_name, ...)                    \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(PERFETTO_TP_VIEW_COLUMN_TYPE, \
                                                 col_name)

// Define a ColumnDataType alias for each column.
#define PERFETTO_TP_VIEW_COLUMN_DATA_TYPE(col_name, ...) \
  using col_name = typename ColumnType::col_name::type;

// Define a ColumnDataType alias for each column in the FROM table.
#define PERFETTO_TP_VIEW_FROM_COLUMN_DATA_TYPE(_, col_name, ...) \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(                \
      PERFETTO_TP_VIEW_COLUMN_DATA_TYPE, col_name)

// Defines an enum member for each column name.
#define PERFETTO_TP_VIEW_COLUMN_ENUM_INDEX(col_name, ...) col_name,

// Defines an enum member for each column name in the FROM table..
#define PERFETTO_TP_VIEW_FROM_COLUMN_ENUM_INDEX(_, col_name, ...) \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(                 \
      PERFETTO_TP_VIEW_COLUMN_ENUM_INDEX, col_name)

// Defines a column index alias for a column in the view.
#define PERFETTO_TP_VIEW_COLUMN_INDEX(col_name, ...) \
  static constexpr uint32_t col_name =               \
      static_cast<uint32_t>(ColumnEnumIndex::col_name);

// Defines a column index alias for a column in the FROM table.
#define PERFETTO_TP_VIEW_FROM_COLUMN_INDEX(_, col_name, ...) \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(            \
      PERFETTO_TP_VIEW_COLUMN_INDEX, col_name)

// Defines a getter in QueryResult for a column in the view.
#define PERFETTO_TP_VIEW_COLUMN_QUERY_RESULT_GETTER(col_name, ...) \
  const ColumnType::col_name& col_name() const {                   \
    return *ColumnType::col_name::FromColumn(                      \
        &columns()[ColumnIndex::col_name]);                        \
  }

// Defines a getter in QueryResult for a column in the FROM table.
#define PERFETTO_TP_VIEW_FROM_COLUMN_QUERY_RESULT_GETTER(_, col_name, ...) \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(                          \
      PERFETTO_TP_VIEW_COLUMN_QUERY_RESULT_GETTER, col_name)

// Defines a getter for the blueprint for each column.
#define PERFETTO_TP_VIEW_COL_BLUEPRINT_GETTER(col_name, ...)                \
  macros_internal::ViewColumnBlueprint<ColumnDataType::col_name> col_name() \
      const {                                                               \
    return macros_internal::ViewColumnBlueprint<ColumnDataType::col_name>(  \
        ColumnIndex::col_name);                                             \
  }

// Defines a getter for the blueprint for each column in the FROM table.
#define PERFETTO_TP_VIEW_FROM_COLUMN_BLUEPRINT_GETTER(_, col_name, ...) \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(                       \
      PERFETTO_TP_VIEW_COL_BLUEPRINT_GETTER, col_name)

// Defines a getter for a column in the RowReference.
#define PERFETTO_TP_VIEW_COLUMN_ROW_REF_GETTER(col_name, ...) \
  ColumnDataType::col_name col_name() const {                 \
    return table_->col_name()[row_number_];                   \
  }
// Defines a getter for a FROM column in the RowReference.
#define PERFETTO_TP_VIEW_FROM_COLUMN_ROW_REF_GETTER(_, col_name, ...) \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(                     \
      PERFETTO_TP_VIEW_COLUMN_ROW_REF_GETTER, col_name)

// Defines a getter for a column in the Iterator.
#define PERFETTO_TP_VIEW_COLUMN_IT_GETTER(col_name, ...)    \
  ColumnDataType::col_name col_name() const {               \
    const auto& col = table_->col_name();                   \
    return col.GetAtIdx(its_[col.overlay_index()].index()); \
  }
// Defines a getter for a FROM column in the RowReference.
#define PERFETTO_TP_VIEW_FROM_COLUMN_IT_GETTER(_, col_name, ...) \
  PERFETTO_TP_VIEW_INVOKE_VIEW_COL_FN_FROM_TABLE(                \
      PERFETTO_TP_VIEW_COLUMN_IT_GETTER, col_name)

// Defines a static assert for ensuring a given join clause is valid.
#define PERFETTO_TP_VIEW_JOIN_STATIC_ASSERT(_, join_table, join_col,       \
                                            prev_table, prev_col, flags)   \
  static_assert(                                                           \
      std::is_same<                                                        \
          TableType::join_table::ColumnType::join_col::type,               \
          TableType::prev_table::ColumnType::prev_col::type>::value ||     \
          (((flags)&View::JoinFlag::kTypeCheckSerialized) != 0 &&          \
           std::is_same<                                                   \
               TableType::join_table::ColumnType::join_col::stored_type,   \
               TableType::prev_table::ColumnType::prev_col::stored_type>:: \
               value),                                                     \
      "Both sides of join do not have the same type; check that you are "  \
      "joining the correct tables and columns.");

#define PERFETTO_TP_VIEW_INTERNAL(view_name, class_name, DEF)                  \
  class class_name : public View {                                             \
   public:                                                                     \
    class RowReference;                                                        \
    class RowNumber;                                                           \
    class Iterator;                                                            \
    class QueryResult;                                                         \
                                                                               \
   private:                                                                    \
    struct TableType {                                                         \
      PERFETTO_TP_VIEW_FROM(DEF, PERFETTO_TP_VIEW_TABLE_TYPE)                  \
      PERFETTO_TP_VIEW_JOINS(DEF, PERFETTO_TP_VIEW_TABLE_TYPE)                 \
      PERFETTO_TP_VIEW_FROM(DEF, PERFETTO_TP_VIEW_TABLE_TYPE_FROM_ALIAS)       \
    };                                                                         \
                                                                               \
    struct TableName {                                                         \
      PERFETTO_TP_VIEW_FROM(DEF, PERFETTO_TP_VIEW_TABLE_NAME)                  \
      PERFETTO_TP_VIEW_JOINS(DEF, PERFETTO_TP_VIEW_TABLE_NAME)                 \
      PERFETTO_TP_VIEW_FROM(DEF, PERFETTO_TP_VIEW_TABLE_NAME_FROM_ALIAS)       \
    };                                                                         \
                                                                               \
    enum class ColumnEnumIndex {                                               \
      PERFETTO_TP_VIEW_COLUMNS(DEF,                                            \
                               PERFETTO_TP_VIEW_FROM_COLUMN_ENUM_INDEX,        \
                               PERFETTO_TP_VIEW_COLUMN_ENUM_INDEX)             \
    };                                                                         \
    struct ColumnType {                                                        \
      PERFETTO_TP_VIEW_COLUMNS(DEF,                                            \
                               PERFETTO_TP_VIEW_FROM_COLUMN_TYPE,              \
                               PERFETTO_TP_VIEW_COLUMN_TYPE)                   \
    };                                                                         \
    struct ColumnDataType {                                                    \
      PERFETTO_TP_VIEW_COLUMNS(DEF,                                            \
                               PERFETTO_TP_VIEW_FROM_COLUMN_DATA_TYPE,         \
                               PERFETTO_TP_VIEW_COLUMN_DATA_TYPE)              \
    };                                                                         \
                                                                               \
    /* Aliases to reduce clutter in class defintions below. */                 \
    using AbstractRowNumber =                                                  \
        macros_internal::AbstractRowNumber<QueryResult, RowReference>;         \
    using AbstractConstRowReference =                                          \
        macros_internal::AbstractConstRowReference<QueryResult, RowNumber>;    \
    using AbstractConstIterator = macros_internal::                            \
        AbstractConstIterator<Iterator, QueryResult, RowNumber, RowReference>; \
                                                                               \
   public:                                                                     \
    struct ColumnIndex {                                                       \
      PERFETTO_TP_VIEW_COLUMNS(DEF,                                            \
                               PERFETTO_TP_VIEW_FROM_COLUMN_INDEX,             \
                               PERFETTO_TP_VIEW_COLUMN_INDEX)                  \
    };                                                                         \
    class RowNumber : public AbstractRowNumber {                               \
     public:                                                                   \
      explicit RowNumber(uint32_t row_number)                                  \
          : AbstractRowNumber(row_number) {}                                   \
    };                                                                         \
    static_assert(std::is_trivially_destructible<RowNumber>::value,            \
                  "Inheritance used without trivial destruction");             \
                                                                               \
    class RowReference : public AbstractConstRowReference {                    \
     public:                                                                   \
      RowReference(const QueryResult* table, uint32_t row_number)              \
          : AbstractConstRowReference(table, row_number) {}                    \
                                                                               \
      PERFETTO_TP_VIEW_COLUMNS(DEF,                                            \
                               PERFETTO_TP_VIEW_FROM_COLUMN_ROW_REF_GETTER,    \
                               PERFETTO_TP_VIEW_COLUMN_ROW_REF_GETTER)         \
    };                                                                         \
    static_assert(std::is_trivially_destructible<RowReference>::value,         \
                  "Inheritance used without trivial destruction");             \
                                                                               \
    class Iterator : public AbstractConstIterator {                            \
     public:                                                                   \
      PERFETTO_TP_VIEW_COLUMNS(DEF,                                            \
                               PERFETTO_TP_VIEW_FROM_COLUMN_IT_GETTER,         \
                               PERFETTO_TP_VIEW_COLUMN_IT_GETTER)              \
                                                                               \
      Iterator& operator++() {                                                 \
        row_number_++;                                                         \
        return AbstractConstIterator::operator++();                            \
      }                                                                        \
                                                                               \
     protected:                                                                \
      /*                                                                       \
       * Must not be public to avoid buggy code because of inheritance         \
       * without virtual destructor.                                           \
       */                                                                      \
      explicit Iterator(const QueryResult* table,                              \
                        std::vector<ColumnStorageOverlay> overlays)            \
          : AbstractConstIterator(table, std::move(overlays)) {}               \
                                                                               \
     private:                                                                  \
      friend class QueryResult;                                                \
      friend class AbstractConstIterator;                                      \
                                                                               \
      uint32_t CurrentRowNumber() const { return row_number_; }                \
                                                                               \
      uint32_t row_number_ = 0;                                                \
    };                                                                         \
                                                                               \
    class QueryResult : public Table {                                         \
     public:                                                                   \
      QueryResult(QueryResult&& other) = default;                              \
      QueryResult& operator=(QueryResult&& other) noexcept = default;          \
                                                                               \
      ~QueryResult() override;                                                 \
                                                                               \
      PERFETTO_TP_VIEW_COLUMNS(                                                \
          DEF,                                                                 \
          PERFETTO_TP_VIEW_FROM_COLUMN_QUERY_RESULT_GETTER,                    \
          PERFETTO_TP_VIEW_COLUMN_QUERY_RESULT_GETTER)                         \
                                                                               \
      class_name::Iterator IterateRows() {                                     \
        return class_name::Iterator(this, CopyOverlays());                     \
      }                                                                        \
                                                                               \
     private:                                                                  \
      friend class class_name;                                                 \
                                                                               \
      QueryResult() = default;                                                 \
      QueryResult(Table&& table) : Table(std::move(table)) {}                  \
    };                                                                         \
                                                                               \
    class_name(                                                                \
        PERFETTO_TP_VIEW_FROM(DEF, PERFETTO_TP_VIEW_CLASS_TABLE_COMMA_FROM)    \
            PERFETTO_TP_VIEW_JOINS(DEF,                                        \
                                   PERFETTO_TP_VIEW_CLASS_TABLE_COMMA_JOIN)    \
                std::nullptr_t = nullptr)                                      \
        : View(PERFETTO_TP_VIEW_FROM(DEF, PERFETTO_TP_VIEW_FROM_PTR_NAME),     \
               {PERFETTO_TP_VIEW_JOINS(DEF, PERFETTO_TP_VIEW_JOIN_DEFN)},      \
               {PERFETTO_TP_VIEW_COLUMNS(DEF,                                  \
                                         PERFETTO_TP_VIEW_FROM_COLUMN_DEFN,    \
                                         PERFETTO_TP_VIEW_COLUMN_DEFN)}) {     \
      PERFETTO_TP_VIEW_JOINS(DEF, PERFETTO_TP_VIEW_JOIN_STATIC_ASSERT)         \
    }                                                                          \
    ~class_name() override;                                                    \
                                                                               \
    QueryResult Query(const std::vector<Constraint>& cs,                       \
                      const std::vector<Order>& ob,                            \
                      const BitVector& cols_used) const {                      \
      QueryResult result;                                                      \
      new (&result) QueryResult(View::Query(cs, ob, cols_used));               \
      return result;                                                           \
    }                                                                          \
                                                                               \
    PERFETTO_TP_VIEW_COLUMNS(DEF,                                              \
                             PERFETTO_TP_VIEW_FROM_COLUMN_BLUEPRINT_GETTER,    \
                             PERFETTO_TP_VIEW_COL_BLUEPRINT_GETTER)            \
                                                                               \
    static const char* Name() { return view_name; }                            \
  }

}  // namespace macros_internal
}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_VIEWS_MACROS_INTERNAL_H_
