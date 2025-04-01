#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_SPECS_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_SPECS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "src/trace_processor/dataframe/type_set.h"

namespace perfetto::trace_processor::dataframe {

// -----------------------------------------------------------------------------
// Column value Types
// -----------------------------------------------------------------------------

// Represents values where the index of the value in the table is the same as
// the value. This allows for zero memory overhead as values don't need to be
// explicitly stored. Operations on column with this type can be highly
// optimized.
struct Id {};

// Represents values where the value is a 32-bit unsigned integer.
struct Uint32 {};

// Represents values where the value is a 32-bit signed integer.
struct Int32 {};

// Represents values where the value is a 64-bit signed integer.
struct Int64 {};

// Represents values where the value is a double.
struct Double {};

// Represents values where the value is a string.
struct String {};

// TypeSet of all possible column value types.
using ColumnType = TypeSet<Id, Uint32, Int32, Int64, Double, String>;

// -----------------------------------------------------------------------------
// Operation Types
// -----------------------------------------------------------------------------

// Filters only cells which compare equal to the given value.
struct Eq {};

// Filters only cells which do not compare equal to the given value.
struct Ne {};

// Filters only cells which are less than the given value.
struct Lt {};

// Filters only cells which are less than or equal to the given value.
struct Le {};

// Filters only cells which are greater than the given value.
struct Gt {};

// Filters only cells which are greater than or equal to the given value.
struct Ge {};

// Filters only cells which match the given glob pattern.
struct Glob {};

// Filters only cells which match the given regex pattern.
struct Regex {};

// Filters only cells which are not NULL.
struct IsNotNull {};

// Filters only cells which are NULL.
struct IsNull {};

// TypeSet of all possible operations for filter conditions.
using Op = TypeSet<Eq, Ne, Lt, Le, Gt, Ge, Glob, Regex, IsNotNull, IsNull>;

// -----------------------------------------------------------------------------
// Sort State Types
// -----------------------------------------------------------------------------

// Represents a column sorted by its id property.
// This is a special state that should only be applied to Id columns, indicating
// the natural ordering where indices equal values.
struct IdSorted {};

// Represents a column which is sorted in ascending order by its value.
struct Sorted {};

// Represents a column which is not sorted.
struct Unsorted {};

// TypeSet of all possible column sort states.
using SortState = TypeSet<IdSorted, Sorted, Unsorted>;

// -----------------------------------------------------------------------------
// Nullability Types
// -----------------------------------------------------------------------------

// Represents a column that doesn't contain NULL values.
struct NonNull {};

// Represents a column that contains NULL values with the storage only
// containing data for non-NULL values.
struct SparseNull {};

// Represents a column that contains NULL values with the storage containing
// data for all values (with undefined values at positions that would be NULL).
struct DenseNull {};

// TypeSet of all possible column nullability states.
using Nullability = TypeSet<NonNull, SparseNull, DenseNull>;

// -----------------------------------------------------------------------------
// Filter Specifications
// -----------------------------------------------------------------------------

// Specifies a filter operation to be applied to column data.
// This is used to generate query plans for filtering rows.
struct FilterSpec {
  // Index of the column in the dataframe to filter.
  uint32_t column_index;

  // Original index from the client query (used for tracking).
  uint32_t source_index;

  // Operation to apply (e.g., equality).
  Op op;

  // Output parameter: index for the filter value in query execution.
  // This is populated during query planning.
  std::optional<uint32_t> value_index;
};

// -----------------------------------------------------------------------------
// Column Specifications
// -----------------------------------------------------------------------------

// Describes the properties of a dataframe column.
struct ColumnSpec {
  // Column name.
  std::string name;

  // Type of content stored in the column.
  ColumnType column_type;

  // Sort order of the column data.
  SortState sort_state;

  // Whether the column can contain NULL values.
  Nullability nullability;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_SPECS_H_
