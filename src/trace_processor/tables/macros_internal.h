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

#include "perfetto/ext/base/small_vector.h"
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
  // This class only exists to allow typechecking to work correctly in Insert
  // below. If we had C++17 and if constexpr, we could statically verify that
  // this was never created but for now, we still need to define it to satisfy
  // the typechecker.
  struct IdAndRow {
    uint32_t id;
  };
  struct RowNumber {
    uint32_t row_number() { PERFETTO_FATAL("Should not be called"); }
  };
  IdAndRow Insert(const Row&) { PERFETTO_FATAL("Should not be called"); }

 private:
  explicit RootParentTable(std::nullptr_t);
};

// IdHelper is used to figure out the Id type for a table.
//
// We do this using templates with the following algorithm:
// 1. If the parent class is anything but RootParentTable, the Id of the
//    table is the same as the Id of the parent.
// 2. If the parent class is RootParentTable (i.e. the table is a root
//    table), then the Id is the one defined in the table itself.
// The net result of this is that all tables in the hierarchy get the
// same type of Id - the one defined in the root table of that hierarchy.
//
// Reasoning: We do this because using uint32_t is very overloaded and
// having a wrapper type for ids is very helpful to avoid confusion with
// row indices (especially because ids and row indices often appear in
// similar places in the codebase - that is at insertion in parsers and
// in trackers).
template <typename ParentClass, typename Class>
struct IdHelper {
  using Id = typename ParentClass::Id;
};
template <typename Class>
struct IdHelper<RootParentTable, Class> {
  using Id = typename Class::DefinedId;
};

// The parent class for all macro generated tables.
// This class is used to extract common code from the macro tables to reduce
// code size.
class MacroTable : public Table {
 protected:
  // Constructors for tables created by the regular constructor.
  MacroTable(StringPool* pool, const Table* parent)
      : Table(pool), allow_inserts_(true), parent_(parent) {
    if (!parent) {
      overlays_.emplace_back();
      columns_.emplace_back(Column::IdColumn(this, 0, 0));
      columns_.emplace_back(
          Column("type", &type_, Column::kNonNull, this, 1, 0));
      return;
    }

    overlays_.resize(parent->overlays().size() + 1);
    for (const Column& col : parent->columns()) {
      columns_.emplace_back(col, this, col.index_in_table(),
                            col.overlay_index());
    }
  }

  // Constructor for tables created by SelectAndExtendParent.
  MacroTable(StringPool* pool,
             const Table& parent,
             const RowMap& parent_overlay)
      : Table(pool), allow_inserts_(false) {
    row_count_ = parent_overlay.size();
    for (const auto& rm : parent.overlays()) {
      overlays_.emplace_back(rm.SelectRows(parent_overlay));
      PERFETTO_DCHECK(overlays_.back().size() == row_count_);
    }
    overlays_.emplace_back(ColumnStorageOverlay(row_count_));

    for (const Column& col : parent.columns()) {
      columns_.emplace_back(col, this, col.index_in_table(),
                            col.overlay_index());
    }
  }
  ~MacroTable() override;

  // We don't want a move or copy constructor because we store pointers to
  // fields of macro tables which will be invalidated if we move/copy them.
  MacroTable(const MacroTable&) = delete;
  MacroTable& operator=(const MacroTable&) = delete;

  MacroTable(MacroTable&&) = delete;
  MacroTable& operator=(MacroTable&&) noexcept = delete;

  void UpdateOverlaysAfterParentInsert() {
    // Add the last inserted row in each of the parent row maps to the
    // corresponding row map in the child.
    for (uint32_t i = 0; i < parent_->overlays().size(); ++i) {
      const ColumnStorageOverlay& parent_rm = parent_->overlays()[i];
      overlays_[i].Insert(parent_rm.Get(parent_rm.size() - 1));
    }
  }

  void UpdateSelfOverlayAfterInsert() {
    // Also add the index of the new row to the identity row map and increment
    // the size.
    overlays_.back().Insert(row_count_++);
  }

  std::vector<ColumnStorageOverlay> FilterAndApplyToOverlays(
      const std::vector<Constraint>& cs,
      RowMap::OptimizeFor optimize_for) const {
    RowMap rm = FilterToRowMap(cs, optimize_for);
    std::vector<ColumnStorageOverlay> overlays;
    overlays.reserve(overlays_.size());
    for (uint32_t i = 0; i < overlays_.size(); ++i) {
      overlays.emplace_back(overlays_[i].SelectRows(rm));
    }
    return overlays;
  }

  // Stores whether inserts are allowed into this macro table; by default
  // inserts are allowed but they are disallowed when a parent table is extended
  // with |ExtendParent|; the rationale for this is that extensions usually
  // happen in dynamic tables and they should not be allowed to insert rows into
  // the real (static) tables.
  bool allow_inserts_ = true;

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
  ColumnStorage<StringPool::Id> type_;

 private:
  const Table* parent_ = nullptr;
};

// Abstract iterator class for macro tables.
// Extracted to allow sharing with view code.
template <typename Iterator,
          typename MacroTable,
          typename RowNumber,
          typename ConstRowReference>
class AbstractConstIterator {
 public:
  explicit operator bool() const { return its_[0]; }

  Iterator& operator++() {
    for (ColumnStorageOverlay::Iterator& it : its_) {
      it.Next();
    }
    return *this_it();
  }

  // Returns a RowNumber for the current row.
  RowNumber row_number() const {
    return RowNumber(this_it()->CurrentRowNumber());
  }

  // Returns a ConstRowReference to the current row.
  ConstRowReference row_reference() const {
    return ConstRowReference(table_, this_it()->CurrentRowNumber());
  }

 protected:
  explicit AbstractConstIterator(const MacroTable* table,
                                 std::vector<ColumnStorageOverlay> overlays)
      : overlays_(std::move(overlays)), table_(table) {
    static_assert(std::is_base_of<Table, MacroTable>::value,
                  "Template param should be a subclass of Table.");

    for (const auto& rm : overlays_) {
      its_.emplace_back(rm.IterateRows());
    }
  }

  // Must not be modified as |its_| contains pointers into this vector.
  std::vector<ColumnStorageOverlay> overlays_;
  std::vector<ColumnStorageOverlay::Iterator> its_;

  const MacroTable* table_;

 private:
  Iterator* this_it() { return static_cast<Iterator*>(this); }
  const Iterator* this_it() const { return static_cast<const Iterator*>(this); }
};

// Abstract RowNumber class for macro tables.
// Extracted to allow sharing with view code.
template <typename MacroTable,
          typename ConstRowReference,
          typename RowReference = void>
class AbstractRowNumber {
 public:
  // Converts this RowNumber to a RowReference for the given |table|.
  template <
      typename RR = RowReference,
      typename = typename std::enable_if<!std::is_same<RR, void>::value>::type>
  RR ToRowReference(MacroTable* table) const {
    return RR(table, row_number_);
  }

  // Converts this RowNumber to a ConstRowReference for the given |table|.
  ConstRowReference ToRowReference(const MacroTable& table) const {
    return ConstRowReference(&table, row_number_);
  }

  // Converts this object to the underlying int value.
  uint32_t row_number() const { return row_number_; }

  // Allows sorting + storage in a map/set.
  bool operator<(const AbstractRowNumber& other) const {
    return row_number_ < other.row_number_;
  }

 protected:
  explicit AbstractRowNumber(uint32_t row_number) : row_number_(row_number) {}

 private:
  uint32_t row_number_ = 0;
};

// Abstract ConstRowReference class for macro tables.
// Extracted to allow sharing with view code.
template <typename MacroTable, typename RowNumber>
class AbstractConstRowReference {
 public:
  // Converts this RowReference to a RowNumber object which is more memory
  // efficient to store.
  RowNumber ToRowNumber() { return RowNumber(row_number_); }

 protected:
  AbstractConstRowReference(const MacroTable* table, uint32_t row_number)
      : table_(table), row_number_(row_number) {}

  const MacroTable* table_ = nullptr;
  uint32_t row_number_ = 0;
};

}  // namespace macros_internal

// Ignore GCC warning about a missing argument for a variadic macro parameter.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif

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

// Used to generate an equality implementation on Table::Row.
#define PERFETTO_TP_ROW_EQUALS(type, name, ...) \
  TypedColumn<type>::Equals(other.name, name)&&

// Defines the parent row field in Insert.
#define PERFETTO_TP_PARENT_ROW_INSERT(type, name, ...) row.name,

// Defines the member variable in the Table.
#define PERFETTO_TP_TABLE_MEMBER(type, name, ...) \
  ColumnStorage<TypedColumn<type>::stored_type> name##_;

#define PERFETTO_TP_COLUMN_FLAG_HAS_FLAG_COL(type, name, flags)               \
  static constexpr uint32_t name##_flags() {                                  \
    return static_cast<uint32_t>(flags) | TypedColumn<type>::default_flags(); \
  }

#define PERFETTO_TP_COLUMN_FLAG_NO_FLAG_COL(type, name) \
  static constexpr uint32_t name##_flags() {            \
    return TypedColumn<type>::default_flags();          \
  }

#define PERFETTO_TP_PARENT_COLUMN_FLAG_HAS_FLAG_COL(type, name, flags) \
  static constexpr uint32_t name##_flags() {                           \
    return (static_cast<uint32_t>(flags) |                             \
            TypedColumn<type>::default_flags()) &                      \
           ~Column::kNoCrossTableInheritFlags;                         \
  }

#define PERFETTO_TP_PARENT_COLUMN_FLAG_NO_FLAG_COL(type, name) \
  static constexpr uint32_t name##_flags() {                   \
    return TypedColumn<type>::default_flags() &                \
           ~Column::kNoCrossTableInheritFlags;                 \
  }

#define PERFETTO_TP_COLUMN_FLAG_CHOOSER(type, name, maybe_flags, fn, ...) fn

// MSVC has slightly different rules about __VA_ARGS__ expansion. This makes it
// behave similarly to GCC/Clang.
// See https://stackoverflow.com/q/5134523/14028266 .
#define PERFETTO_TP_EXPAND_VA_ARGS(x) x

#define PERFETTO_TP_COLUMN_FLAG(...)                          \
  PERFETTO_TP_EXPAND_VA_ARGS(PERFETTO_TP_COLUMN_FLAG_CHOOSER( \
      __VA_ARGS__, PERFETTO_TP_COLUMN_FLAG_HAS_FLAG_COL,      \
      PERFETTO_TP_COLUMN_FLAG_NO_FLAG_COL)(__VA_ARGS__))

#define PERFETTO_TP_PARENT_COLUMN_FLAG(...)                     \
  PERFETTO_TP_EXPAND_VA_ARGS(PERFETTO_TP_COLUMN_FLAG_CHOOSER(   \
      __VA_ARGS__, PERFETTO_TP_PARENT_COLUMN_FLAG_HAS_FLAG_COL, \
      PERFETTO_TP_PARENT_COLUMN_FLAG_NO_FLAG_COL)(__VA_ARGS__))

// Creates the sparse vector with the given flags.
#define PERFETTO_TP_TABLE_CONSTRUCTOR_SV(type, name, ...)        \
  name##_(ColumnStorage<TypedColumn<type>::stored_type>::Create< \
          (name##_flags() & Column::Flag::kDense) != 0>()),

// Invokes the chosen column constructor by passing the given args.
#define PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN(type, name, ...)   \
  columns_.emplace_back(#name, &name##_, name##_flags(), this,  \
                        static_cast<uint32_t>(columns_.size()), \
                        static_cast<uint32_t>(overlays_.size()) - 1);

// Inserts the value into the corresponding column.
#define PERFETTO_TP_COLUMN_APPEND(type, name, ...) \
  mutable_##name()->Append(std::move(row.name));

// Creates a schema entry for the corresponding column.
#define PERFETTO_TP_COLUMN_SCHEMA(type, name, ...)               \
  schema.columns.emplace_back(Table::Schema::Column{             \
      #name, TypedColumn<type>::SqlValueType(), false,           \
      static_cast<bool>(name##_flags() & Column::Flag::kSorted), \
      static_cast<bool>(name##_flags() & Column::Flag::kHidden), \
      static_cast<bool>(name##_flags() & Column::Flag::kSetId)});

// Defines the immutable accessor for a column.
#define PERFETTO_TP_TABLE_COL_GETTER(type, name, ...)                          \
  const TypedColumn<type>& name() const {                                      \
    return static_cast<const TypedColumn<type>&>(columns_[ColumnIndex::name]); \
  }

// Defines the accessors for a column.
#define PERFETTO_TP_TABLE_MUTABLE_COL_GETTER(type, name, ...)             \
  TypedColumn<type>* mutable_##name() {                                   \
    return static_cast<TypedColumn<type>*>(&columns_[ColumnIndex::name]); \
  }

// Defines the accessors for a column.
#define PERFETTO_TP_TABLE_STATIC_ASSERT_FLAG(type, name, ...)                \
  static_assert(Column::IsFlagsAndTypeValid<TypedColumn<type>::stored_type>( \
                    name##_flags()),                                         \
                "Column type and flag combination is not valid");

// Defines the parameter for the |ExtendParent| function.
#define PERFETTO_TP_TABLE_EXTEND_PARAM(type, name, ...) \
  ColumnStorage<TypedColumn<type>::stored_type> name,

// Defines the parameter passing for the |ExtendParent| function.
#define PERFETTO_TP_TABLE_EXTEND_PARAM_PASSING(type, name, ...) std::move(name),

// Sets the table nullable vector to the parameter passed in the
// |SelectAndExtendParent| function.
#define PERFETTO_TP_TABLE_EXTEND_SET_NV(type, name, ...) \
  PERFETTO_DCHECK(name.size() == parent_overlay.size()); \
  name##_ = std::move(name);

// Definition used as the parent of root tables.
#define PERFETTO_TP_ROOT_TABLE_PARENT_DEF(NAME, PARENT, C) \
  NAME(macros_internal::RootParentTable, "root")

// Defines the getter for the column value in the RowReference.
#define PERFETTO_TP_TABLE_CONST_ROW_REF_GETTER(type, name, ...) \
  type name() const { return table_->name()[row_number_]; }

// Defines the accessor for the column value in the RowReference.
#define PERFETTO_TP_TABLE_ROW_REF_SETTER(type, name, ...)          \
  void set_##name(TypedColumn<type>::non_optional_type v) const {  \
    return mutable_table()->mutable_##name()->Set(row_number_, v); \
  }

// Defines the getter for the column value in the ConstIterator.
#define PERFETTO_TP_TABLE_CONST_IT_GETTER(type, name, ...)  \
  type name() const {                                       \
    const auto& col = table_->name();                       \
    return col.GetAtIdx(its_[col.overlay_index()].index()); \
  }

// Defines the setter for the column value in the Iterator.
#define PERFETTO_TP_TABLE_IT_SETTER(type, name, ...)        \
  void set_##name(TypedColumn<type>::non_optional_type v) { \
    auto* col = mutable_table_->mutable_##name();           \
    col->SetAtIdx(its_[col->overlay_index()].index(), v);   \
  }

// Defines the column index constexpr declaration.
#define PERFETTO_TP_COLUMN_INDEX(type, name, ...) \
  static constexpr uint32_t name = static_cast<uint32_t>(ColumnIndexEnum::name);

// Defines an alias for column type for each column.
#define PERFETTO_TP_COLUMN_TYPE_USING(type, name, ...) \
  using name = TypedColumn<type>;

// Calls ShrinkToFit on each column.
#define PERFETTO_TP_COLUMN_SHRINK_TO_FIT(type, name, ...) name##_.ShrinkToFit();

// For more general documentation, see PERFETTO_TP_TABLE in macros.h.
#define PERFETTO_TP_TABLE_INTERNAL(table_name, class_name, parent_class_name, \
                                   DEF)                                       \
  class class_name : public macros_internal::MacroTable {                     \
   public:                                                                    \
    /* Forward declaration to allow free usage below. */                      \
    class ConstRowReference;                                                  \
    class RowReference;                                                       \
    class RowNumber;                                                          \
    class ConstIterator;                                                      \
                                                                              \
   private:                                                                   \
    /*                                                                        \
     * Allows IdHelper to access DefinedId for root tables.                   \
     * Needs to be defined here to allow the public using declaration of Id   \
     * below to work correctly.                                               \
     */                                                                       \
    friend struct macros_internal::IdHelper<parent_class_name, class_name>;   \
                                                                              \
    /* Whether or not this is a root table */                                 \
    static constexpr bool kIsRootTable =                                      \
        std::is_same<parent_class_name,                                       \
                     macros_internal::RootParentTable>::value;                \
                                                                              \
    /* Aliases to reduce clutter in class defintions below. */                \
    using AbstractRowNumber = macros_internal::                               \
        AbstractRowNumber<class_name, ConstRowReference, RowReference>;       \
    using AbstractConstRowReference =                                         \
        macros_internal::AbstractConstRowReference<class_name, RowNumber>;    \
    using AbstractConstIterator =                                             \
        macros_internal::AbstractConstIterator<ConstIterator,                 \
                                               class_name,                    \
                                               RowNumber,                     \
                                               ConstRowReference>;            \
                                                                              \
    enum class ColumnIndexEnum {                                              \
      id,                                                                     \
      type, /* Expands to col1, col2, ... */                                  \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_NAME_COMMA) kNumCols           \
    };                                                                        \
                                                                              \
    /*                                                                        \
     * Defines a new id type for a hierarchy of tables.                       \
     * We define it here as we need this type to be visible for the public    \
     * using declaration of Id below.                                         \
     * Note: This type will only used if this table is a root table.          \
     */                                                                       \
    struct DefinedId : public BaseId {                                        \
      DefinedId() = default;                                                  \
      explicit constexpr DefinedId(uint32_t v) : BaseId(v) {}                 \
    };                                                                        \
    static_assert(std::is_trivially_destructible<DefinedId>::value,           \
                  "Inheritance used without trivial destruction");            \
                                                                              \
    static constexpr uint32_t id_flags() { return Column::kIdFlags; }         \
    static constexpr uint32_t type_flags() { return Column::kNoFlag; }        \
    PERFETTO_TP_PARENT_COLUMNS(DEF, PERFETTO_TP_PARENT_COLUMN_FLAG)           \
    PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_COLUMN_FLAG)                   \
                                                                              \
   public:                                                                    \
    /*                                                                        \
     * This defines the type of the id to be the type of the root             \
     * table of the hierarchy - see IdHelper for more details.                \
     */                                                                       \
    using Id = macros_internal::IdHelper<parent_class_name, class_name>::Id;  \
                                                                              \
    struct ColumnIndex {                                                      \
      static constexpr uint32_t id =                                          \
          static_cast<uint32_t>(ColumnIndexEnum::id);                         \
      static constexpr uint32_t type =                                        \
          static_cast<uint32_t>(ColumnIndexEnum::type);                       \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_COLUMN_INDEX)                  \
    };                                                                        \
                                                                              \
    struct ColumnType {                                                       \
      using id = IdColumn<Id>;                                                \
      using type = TypedColumn<StringPool::Id>;                               \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_COLUMN_TYPE_USING)             \
    };                                                                        \
                                                                              \
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
        /*                                                                    \
         * Expands to                                                         \
         * col1 = col1_c;                                                     \
         * ...                                                                \
         */                                                                   \
        PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_ROW_INITIALIZER)           \
      }                                                                       \
                                                                              \
      bool operator==(const class_name::Row& other) const {                   \
        return PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_ROW_EQUALS) true;     \
      }                                                                       \
                                                                              \
      /*                                                                      \
       * Expands to                                                           \
       * col_type1 col1 = {};                                                 \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_ROW_DEFINITION)              \
    };                                                                        \
    static_assert(std::is_trivially_destructible<Row>::value,                 \
                  "Inheritance used without trivial destruction");            \
                                                                              \
    /*                                                                        \
     * Reference to a row which exists in the table.                          \
     *                                                                        \
     * Allows caller code to store and instances of this object without       \
     * having to interact with row numbers.                                   \
     */                                                                       \
    class ConstRowReference : public AbstractConstRowReference {              \
     public:                                                                  \
      ConstRowReference(const class_name* table, uint32_t row_number)         \
          : AbstractConstRowReference(table, row_number) {}                   \
                                                                              \
      PERFETTO_TP_TABLE_CONST_ROW_REF_GETTER(Id, id)                          \
      PERFETTO_TP_TABLE_CONST_ROW_REF_GETTER(StringPool::Id, type)            \
                                                                              \
      /*                                                                      \
       * Expands to                                                           \
       * col1_type col1() const { return table_->col1()[row_]; }              \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_CONST_ROW_REF_GETTER)    \
    };                                                                        \
    static_assert(std::is_trivially_destructible<ConstRowReference>::value,   \
                  "Inheritance used without trivial destruction");            \
                                                                              \
    /*                                                                        \
     * Reference to a row which exists in the table.                          \
     *                                                                        \
     * Allows caller code to store and instances of this object without       \
     * having to interact with row numbers.                                   \
     */                                                                       \
    class RowReference : public ConstRowReference {                           \
     public:                                                                  \
      RowReference(class_name* table, uint32_t row_number)                    \
          : ConstRowReference(table, row_number) {}                           \
                                                                              \
      /*                                                                      \
       * Expands to                                                           \
       * void set_col1(col1_type v) { table_->mutable_col1()->Set(row, v); }  \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_ROW_REF_SETTER)          \
                                                                              \
     private:                                                                 \
      class_name* mutable_table() const {                                     \
        return const_cast<class_name*>(table_);                               \
      }                                                                       \
    };                                                                        \
    static_assert(std::is_trivially_destructible<RowReference>::value,        \
                  "Inheritance used without trivial destruction");            \
                                                                              \
    /*                                                                        \
     * Strongly typed wrapper around the row index. Prefer storing this over  \
     * storing RowReference to reduce memory usage                            \
     */                                                                       \
    class RowNumber : public AbstractRowNumber {                              \
     public:                                                                  \
      explicit RowNumber(uint32_t row_number)                                 \
          : AbstractRowNumber(row_number) {}                                  \
    };                                                                        \
    static_assert(std::is_trivially_destructible<RowNumber>::value,           \
                  "Inheritance used without trivial destruction");            \
                                                                              \
    /* Return value of Insert giving access to id and row number */           \
    struct IdAndRow {                                                         \
      Id id;                                                                  \
      uint32_t row;                                                           \
      RowReference row_reference;                                             \
      RowNumber row_number;                                                   \
    };                                                                        \
                                                                              \
    /*                                                                        \
     * Strongly typed const iterator for this macro table.                    \
     *                                                                        \
     * Allows efficient retrieval of values from this table without having to \
     * deal with row numbers, ColumnStorageOverlays or indices.               \
     */                                                                       \
    class ConstIterator : public AbstractConstIterator {                      \
     public:                                                                  \
      PERFETTO_TP_TABLE_CONST_IT_GETTER(Id, id)                               \
      PERFETTO_TP_TABLE_CONST_IT_GETTER(StringPool::Id, type)                 \
                                                                              \
      /*                                                                      \
       * Expands to                                                           \
       * col1_type col1() const { return table_->col1().GetAtIdx(i); }        \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_CONST_IT_GETTER)         \
                                                                              \
     protected:                                                               \
      /*                                                                      \
       * Must not be public to avoid buggy code because of inheritance        \
       * without virtual destructor.                                          \
       */                                                                     \
      explicit ConstIterator(const class_name* table,                         \
                             std::vector<ColumnStorageOverlay> overlays)      \
          : AbstractConstIterator(table, std::move(overlays)) {}              \
                                                                              \
      uint32_t CurrentRowNumber() const {                                     \
        /*                                                                    \
         * Because the last ColumnStorageOverlay belongs to this table it     \
         * will be dense (i.e. every row in the table will be part of this    \
         * ColumnStorageOverlay + will be represented with a range). This     \
         * means that the index() of the last ColumnStorageOverlay iterator   \
         * is precisely the row number in table!                              \
         */                                                                   \
        return its_.back().index();                                           \
      }                                                                       \
                                                                              \
     private:                                                                 \
      friend class class_name;                                                \
      friend class AbstractConstIterator;                                     \
    };                                                                        \
                                                                              \
    /*                                                                        \
     * Strongly typed iterator for this macro table.                          \
     *                                                                        \
     * Enhances ConstIterator by also allowing values in the table to be set  \
     * as well as retrieved.                                                  \
     */                                                                       \
    class Iterator : public ConstIterator {                                   \
     public:                                                                  \
      /*                                                                      \
       * Expands to                                                           \
       * void set_col1(col1_type v) { table_->mut_col1()->SetAtIdx(i, v); }   \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_IT_SETTER)               \
                                                                              \
      /*                                                                      \
       * Returns a RowReference to the current row.                           \
       */                                                                     \
      RowReference row_reference() const {                                    \
        return RowReference(mutable_table_, CurrentRowNumber());              \
      }                                                                       \
                                                                              \
     private:                                                                 \
      friend class class_name;                                                \
                                                                              \
      /*                                                                      \
       * Must not be public to avoid buggy code because of inheritance        \
       * without virtual destructor.                                          \
       */                                                                     \
      explicit Iterator(class_name* table,                                    \
                        std::vector<ColumnStorageOverlay> overlays)           \
          : ConstIterator(table, std::move(overlays)),                        \
            mutable_table_(table) {}                                          \
                                                                              \
      class_name* mutable_table_ = nullptr;                                   \
    };                                                                        \
                                                                              \
    class_name(StringPool* pool, parent_class_name* parent)                   \
        : macros_internal::MacroTable(pool, parent),                          \
          PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_CONSTRUCTOR_SV)    \
              parent_(parent) {                                               \
      PERFETTO_CHECK(kIsRootTable == (parent == nullptr));                    \
                                                                              \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_STATIC_ASSERT_FLAG)      \
                                                                              \
      /*                                                                      \
       * Expands to                                                           \
       * columns_.emplace_back("col1", col1_, Column::kNoFlag, this,          \
       *                       static_cast<uint32_t>(columns_.size()),        \
       *                       static_cast<uint32_t>(overlays_.size()) - 1);  \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN);   \
    }                                                                         \
    ~class_name() override;                                                   \
                                                                              \
    IdAndRow Insert(const Row& row) {                                         \
      PERFETTO_DCHECK(allow_inserts_);                                        \
                                                                              \
      Id id;                                                                  \
      uint32_t row_number = row_count();                                      \
      if (kIsRootTable) {                                                     \
        id = Id{row_number};                                                  \
        type_.Append(string_pool_->InternString(row.type()));                 \
      } else {                                                                \
        PERFETTO_DCHECK(parent_);                                             \
        id = Id{parent_->Insert(row).id};                                     \
        UpdateOverlaysAfterParentInsert();                                    \
      }                                                                       \
                                                                              \
      /*                                                                      \
       * Expands to                                                           \
       * col1_.Append(row.col1);                                              \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_COLUMN_APPEND);              \
                                                                              \
      UpdateSelfOverlayAfterInsert();                                         \
      return {id, row_number, RowReference(this, row_number),                 \
              RowNumber(row_number)};                                         \
    }                                                                         \
                                                                              \
    static Table::Schema ComputeStaticSchema() {                              \
      Table::Schema schema;                                                   \
      schema.columns.emplace_back(Table::Schema::Column{                      \
          "id", SqlValue::Type::kLong, true, true, false, false});            \
      schema.columns.emplace_back(Table::Schema::Column{                      \
          "type", SqlValue::Type::kString, false, false, false, false});      \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_COLUMN_SCHEMA);                \
      return schema;                                                          \
    }                                                                         \
                                                                              \
    void ShrinkToFit() {                                                      \
      type_.ShrinkToFit();                                                    \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_COLUMN_SHRINK_TO_FIT);       \
    }                                                                         \
                                                                              \
    /* Iterates the table. */                                                 \
    ConstIterator IterateRows() const {                                       \
      return ConstIterator(this, CopyOverlays());                             \
    }                                                                         \
                                                                              \
    /* Iterates the table. */                                                 \
    Iterator IterateRows() { return Iterator(this, CopyOverlays()); }         \
                                                                              \
    /* Filters the Table using the specified filter constraints. */           \
    ConstIterator FilterToIterator(                                           \
        const std::vector<Constraint>& cs,                                    \
        RowMap::OptimizeFor opt = RowMap::OptimizeFor::kMemory) const {       \
      return ConstIterator(this, FilterAndApplyToOverlays(cs, opt));          \
    }                                                                         \
                                                                              \
    /* Filters the Table using the specified filter constraints. */           \
    Iterator FilterToIterator(                                                \
        const std::vector<Constraint>& cs,                                    \
        RowMap::OptimizeFor opt = RowMap::OptimizeFor::kMemory) {             \
      return Iterator(this, FilterAndApplyToOverlays(cs, opt));               \
    }                                                                         \
                                                                              \
    /* Returns a ConstRowReference to the row pointed to by |find_id|. */     \
    base::Optional<ConstRowReference> FindById(Id find_id) const {            \
      base::Optional<uint32_t> row = id().IndexOf(find_id);                   \
      if (!row)                                                               \
        return base::nullopt;                                                 \
      return ConstRowReference(this, *row);                                   \
    }                                                                         \
                                                                              \
    /* Returns a RowReference to the row pointed to by |find_id|. */          \
    base::Optional<RowReference> FindById(Id find_id) {                       \
      base::Optional<uint32_t> row = id().IndexOf(find_id);                   \
      if (!row)                                                               \
        return base::nullopt;                                                 \
      return RowReference(this, *row);                                        \
    }                                                                         \
                                                                              \
    const IdColumn<Id>& id() const {                                          \
      return static_cast<const IdColumn<Id>&>(                                \
          columns_[static_cast<uint32_t>(ColumnIndex::id)]);                  \
    }                                                                         \
    PERFETTO_TP_TABLE_COL_GETTER(StringPool::Id, type)                        \
                                                                              \
    /* Returns the name of the table */                                       \
    static constexpr const char* Name() { return table_name; }                \
                                                                              \
    /*                                                                        \
     * Creates a filled instance of this class by selecting all rows in       \
     * parent and filling the table columns with the provided vectors.        \
     */                                                                       \
    static std::unique_ptr<Table> ExtendParent(                               \
        const parent_class_name& parent,                                      \
        PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_EXTEND_PARAM)        \
            std::nullptr_t = nullptr) {                                       \
      return std::unique_ptr<Table>(new class_name(                           \
          parent.string_pool(), parent, RowMap(0, parent.row_count()),        \
          PERFETTO_TP_TABLE_COLUMNS(                                          \
              DEF, PERFETTO_TP_TABLE_EXTEND_PARAM_PASSING) nullptr));         \
    }                                                                         \
                                                                              \
    /*                                                                        \
     * Creates a filled instance of this class by first selecting all rows in \
     * parent given by |rows| and filling the table columns with the provided \
     * vectors.                                                               \
     */                                                                       \
    static std::unique_ptr<Table> SelectAndExtendParent(                      \
        const parent_class_name& parent,                                      \
        std::vector<parent_class_name::RowNumber> parent_row_overlay,         \
        PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_EXTEND_PARAM)        \
            std::nullptr_t = nullptr) {                                       \
      std::vector<uint32_t> prs_untyped(parent_row_overlay.size());           \
      for (uint32_t i = 0; i < parent_row_overlay.size(); ++i) {              \
        prs_untyped[i] = parent_row_overlay[i].row_number();                  \
      }                                                                       \
      return std::unique_ptr<Table>(new class_name(                           \
          parent.string_pool(), parent, RowMap(std::move(prs_untyped)),       \
          PERFETTO_TP_TABLE_COLUMNS(                                          \
              DEF, PERFETTO_TP_TABLE_EXTEND_PARAM_PASSING) nullptr));         \
    }                                                                         \
                                                                              \
    /*                                                                        \
     * Expands to                                                             \
     * const TypedColumn<col1_type>& col1() { return col1_; }                 \
     * ...                                                                    \
     */                                                                       \
    PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_COL_GETTER)                \
                                                                              \
    /*                                                                        \
     * Expands to                                                             \
     * TypedColumn<col1_type>* mutable_col1() { return &col1_; }              \
     * ...                                                                    \
     */                                                                       \
    PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_MUTABLE_COL_GETTER)        \
                                                                              \
   private:                                                                   \
    class_name(StringPool* pool,                                              \
               const parent_class_name& parent,                               \
               RowMap parent_overlay,                                         \
               PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_EXTEND_PARAM) \
                   std::nullptr_t = nullptr)                                  \
        : macros_internal::MacroTable(pool, parent, parent_overlay) {         \
      PERFETTO_TP_ALL_COLUMNS(DEF, PERFETTO_TP_TABLE_STATIC_ASSERT_FLAG)      \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_EXTEND_SET_NV)         \
                                                                              \
      /*                                                                      \
       * Expands to                                                           \
       * columns_.emplace_back("col1", col1_, Column::kNoFlag, this,          \
       *                       static_cast<uint32_t>(columns_.size()),        \
       *                       static_cast<uint32_t>(overlays_.size()) - 1);  \
       * ...                                                                  \
       */                                                                     \
      PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_CONSTRUCTOR_COLUMN);   \
    }                                                                         \
                                                                              \
    /*                                                                        \
     * Expands to                                                             \
     * NullableVector<col1_type> col1_;                                       \
     * ...                                                                    \
     */                                                                       \
    PERFETTO_TP_TABLE_COLUMNS(DEF, PERFETTO_TP_TABLE_MEMBER)                  \
                                                                              \
    parent_class_name* parent_ = nullptr;                                     \
  }

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TABLES_MACROS_INTERNAL_H_
