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

#ifndef SRC_TRACE_PROCESSOR_DB_VIEW_H_
#define SRC_TRACE_PROCESSOR_DB_VIEW_H_

#include <stdint.h>

#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/trace_processor/iterator.h"
#include "src/trace_processor/containers/bit_vector.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/table.h"

namespace perfetto {
namespace trace_processor {

// Implementation of a "SQL view" on top of trace processor
// columnar tables.
//
// Supported operations are:
// 1) joining tables together by id
// 2) exporting columns with different names
//
// Note: unlike traditional SQL views, this class does *not*
// allow arbitrary joins. Instead, it only supports joins between
// tables on ids and only supports a single constraint per table.
//
// Concretely, suppose you have two tables A and B with A having
// a column named b_id containing references to rows in B. This
// class allows defining a view AB which contains the rows of A,
// transparently extended with the columns from B.
//
// We implement this specially in trace processor instead of doing
// this in SQL for a few reasons:
//  1) The views we write using this class are used in highly
//     performance sensitive contexts so avoiding the "virtual table"
//     overhead from SQLite makes a meaningful difference.
//  2) In trace processor, we have more knowledge of the semantics
//     of tables (i.e. ids are unique, sorted and non-null). While
//     we can expose knowledge of some of this context of SQLite,
//     it will never do as good a job of ordering joins as we can
//     do ourselves.
//  3) By looking at which columns are used, we can potentially skip
//     filtering/sorting some tables in the join which can massively
//     speed up queries. Because SQLite lacks the semantic knowledge
//     (see 2), it refuses to skip any join as rows could potentially
//     be filtered out (even though, we know they wouldn't be).
//
// Design doc: go/perfetto-cpp-views
class View {
 public:
  // Bitflags which can be set to modify how joins between tables are
  // performed. Multiple flags can be set by bitwise-oring them together.
  enum JoinFlag : uint32_t {
    // Flag to be specified if the join has no special properties. That is
    // the join is standard inner join.
    kNoFlag = 0,

    // Indicates that the right hand side of the join is for a column containing
    // strongly typed ids but the left side only contains serialized uint32_t.
    // This means both sides will be type-checked based on serialized types
    // rather than actual column types.
    //
    // This flag is not utilized by this class but any wrapping logic (e.g. the
    // view macros) can make use of this to have very strict type checking
    // except where this flag is specified.
    //
    // The motivation for this flag comes from thread/process table where we
    // use uint32_t as ids for these tables: this is because this was standard
    // convention before typechecked tables which we didn't change because it
    // was a) too much effort to change b) made the code messier (as UniqueTid
    // and UniquePid are used as indices into vectors in several places in trace
    // processor).
    kTypeCheckSerialized = 1 << 0,

    // Indicates that the right table's id column will contain every potential
    // id which can appear in the left table.
    //
    // As a rule of thumb, this flag can be specified whenever the right table
    // is a "root" table; it's possible that there are other cases but this
    // would need case-by-case consideration.
    kIdAlwaysPresent = 1 << 0,
  };

  // References a new table which should be introduced into the view by joining
  // it with an existing table.
  //
  // Note that all |const char*| varaibles below should be long lived string
  // literals (generally coming from macro defintions).
  struct JoinTable {
    // The table which is being joined into this view. This table will be
    // on the RHS of the join.
    Table* table;

    // The name of this table; only used to allow referencing this table in
    // later |Join| structs.
    const char* table_name;

    // The name of the id column |table|.
    // Note: in practice this will always be "id" but we allow specifiying it
    // to allow generality.
    const char* col;

    // A table previously introduced into this view which will act as the LHS of
    // the join.
    const char* prev_table_name;

    // The name of the column in the table given by |prev_table_name| which will
    // contain ids for |table|.
    const char* prev_col;

    // Set of bitwise-ORed flags modifying how the join should be perfomed. See
    // |JoinFlag| enum for potential flags.
    uint32_t join_flags;
  };

  // Stores information about an output column for this view.
  struct OutputColumn {
    // The name of the column being exposed.
    const char* col_name;

    // The name of the source table this column comes from.
    const char* source_table_name;

    // The name of the column in the source table.
    const char* source_col_name;
  };

  View();

  View(const View&) noexcept = delete;
  View& operator=(const View&) = delete;

  View(View&&) noexcept = default;
  View& operator=(View&&) = default;

  virtual ~View();

  // Creates a new View from the given parameters.
  base::Status Create(Table* root_table,
                      const char* root_table_name,
                      std::initializer_list<JoinTable> joins,
                      std::initializer_list<OutputColumn> columns,
                      View* view);

  Table Query(const std::vector<Constraint>& cs,
              const std::vector<Order>& ob,
              const BitVector& cols_used) const;

  uint32_t GetColumnCount() const;
  uint32_t EstimateRowCount() const;

  const Table::Schema& schema() const { return schema_; }

 protected:
  // Constructor variant of Create, exposed for subclasses; any errors will
  // simply be PERFETTO_FATAL-ed.
  View(Table* root_table,
       const char* root_table_name,
       std::initializer_list<JoinTable> joins,
       std::initializer_list<OutputColumn> columns);

 private:
  // The tables participating in a view laid out in a tree structure.
  //
  // The parent represents the LHS of the join with each child being a separate
  // table being joined on the RHS of the join. This structure allows enforces,
  // at the type-system level, that each joined table has preceisely one join
  // condition.
  //
  // Note, however, that the same table pointer *can* appear multiple times in
  // different parts of the tree but only when the "name" of the table is also
  // different (by having a different name we can disambiguate which column we
  // need to choose when constructing the final output table).
  struct TableNode {
    /// The table for the root of this tree.
    //
    // For all except the root node, this table will always be on the right side
    // of the join for its parent and the left side of the join for any nodes in
    // |children|.
    Table* table;

    // The index of the id column in |table|.
    // In practice, this will always be zero (as id columns are implicitly the
    // first column) but having this allows flexibility for the future.
    std::optional<uint32_t> join_col_idx;

    // The index of the column in the parent table which is selecting the rows
    // in |table|.
    std::optional<uint32_t> parent_join_col_idx;

    // Set of bitwise-ORed flags modifying how the join should be perfomed. See
    // |JoinFlag| struct for potential flags.
    uint32_t join_flags;

    // The child tables participating in the join.
    using Children = base::SmallVector<std::unique_ptr<TableNode>, 4>;
    Children children;
  };
  using SourceColumn = std::pair<const TableNode*, uint32_t /* column_idx */>;

  // Helper class for performing the join algorithm.
  //
  // This is useful to split up the algorithm into functions without having to
  // constantly pass the state data structures between functions.
  class QueryHelper {
   public:
    // Contains transient state about a single table which is used while
    // querying a view.
    struct NodeState {
      // "Input" parameters.
      // The following are set by BuildNodeStateMap and used in the other
      // functions.

      // The set of filter constraints on this table.
      std::vector<Constraint> cs;

      // Whether any column from this table is used by SQLite or if this table
      // is an ancestor of such a table.
      bool is_used;

      // Whether joining this table with its parent can cause rows to be removed
      // from the parent. This is true either if:
      // 1) this table is filtered (i.e. |cs| is not empty).
      // 2) this table does not have every id (i.e. it's not a root table)
      // 3) this table is an ancestor of a table which |removes_parent_rows|.
      bool removes_parent_rows;

      // "Output" parameters.
      // These are modified throughout every function and will be incrementally
      // refined to the final until used to build the output table in
      // |BuildTable|.

      // The current output table. At the end of |FilterAndJoinRecursive|, this
      // contains the table
      Table output;

      // The current RowMap which needs to be applied to |output| to accurately
      // join with the parent. Built by |FilterAndJoinRecursive| and applied
      // recursively downwards in |ApplyRowMapRecursive|
      RowMap parent_join_rm;
    };
    using NodeStateMap = base::FlatHashMap<const TableNode*, NodeState>;

    QueryHelper(TableNode* root_node,
                const std::vector<SourceColumn>&,
                const std::vector<Constraint>&,
                const BitVector& cols_used);

    // See definition of View::Query for information about these functions.
    static NodeStateMap BuildNodeStateMap(TableNode* root_node,
                                          const std::vector<SourceColumn>&,
                                          const std::vector<Constraint>&,
                                          const BitVector& cols_used);
    void FilterAndJoinRecursive(TableNode* node);
    void ApplyRowMapRecursive(TableNode* root) {
      // To avoid the root node parent_rm emptying it, create a RowMap which
      // will simply select all the rows.
      auto& root_state = *state_.Find(root);
      return ApplyRowMapRecursive(root,
                                  RowMap(0, root_state.output.row_count()));
    }
    Table BuildTable(TableNode* root,
                     const Table::Schema& schema,
                     const std::vector<SourceColumn>&,
                     const BitVector& cols_used);

   private:
    void ApplyRowMapRecursive(TableNode* node, RowMap rm);

    NodeStateMap state_;
  };

  View(std::unique_ptr<TableNode> root,
       std::vector<SourceColumn>,
       Table::Schema schema);

  // Implements a post-order DFS on the |TableNode| struct. Useful for
  // compactly writing a tree traversal with a focus on what's happening.
  template <typename Fn>
  static void PostOrderDfs(TableNode* node, Fn fn) {
    for (const auto& child : node->children) {
      PostOrderDfs(child.get(), fn);
    }
    fn(node);
  }

  std::unique_ptr<TableNode> root_node_;
  std::vector<SourceColumn> source_col_by_output_idx_;
  Table::Schema schema_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_DB_VIEW_H_
