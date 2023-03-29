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

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/containers/row_map.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/table.h"
#include "src/trace_processor/db/typed_column.h"

namespace perfetto {
namespace trace_processor {

View::View() = default;

View::View(std::unique_ptr<TableNode> root,
           std::vector<SourceColumn> source_col_by_output_idx,
           Table::Schema schema)
    : root_node_(std::move(root)),
      source_col_by_output_idx_(std::move(source_col_by_output_idx)),
      schema_(std::move(schema)) {}

View::~View() = default;

base::Status View::Create(Table* root_table,
                          const char* root_table_name,
                          std::initializer_list<JoinTable> joins,
                          std::initializer_list<OutputColumn> cols,
                          View* view) {
  // Insert the node for the root table; the column indices being std::nullopt
  // indicates this is the root.
  std::unique_ptr<TableNode> root_node(
      new TableNode{root_table, std::nullopt, std::nullopt, JoinFlag::kNoFlag,
                    TableNode::Children{}});
  base::FlatHashMap<base::StringView, TableNode*> node_map;
  node_map.Insert(root_table_name, root_node.get());

  // Verify that all the joins are well-formed and build the join-tree
  // structure.
  for (const JoinTable& join : joins) {
    // Verify that the previous table was previously defined (either by
    // the root or prior join).
    TableNode** prev_node_it = node_map.Find(join.prev_table_name);
    if (!prev_node_it) {
      return base::ErrStatus(
          "View has table %s joining with table %s which was not "
          "previously defined",
          join.table_name, join.prev_table_name);
    }
    TableNode* prev_node = *prev_node_it;

    // Verify that the previous table's column exists.
    std::optional<uint32_t> opt_prev_col_idx =
        prev_node->table->GetColumnIndexByName(join.prev_col);
    if (!opt_prev_col_idx) {
      return base::ErrStatus(
          "View references column %s in table %s which does not exist",
          join.prev_col, join.prev_table_name);
    }

    // Verify that the current table's column exists.
    std::optional<uint32_t> opt_col_idx =
        join.table->GetColumnIndexByName(join.col);
    if (!opt_col_idx) {
      return base::ErrStatus(
          "View references column %s in table %s which does not exist",
          join.col, join.table_name);
    }

    // TODO(lalitm): add some extra checks about the columns being joined here
    // (i.e. right column being an id, left column being non-nullable, neither
    // column being a dummy column etc, neither column is hidden etc.).

    // Build the node and insert it into the map.
    prev_node->children.emplace_back(
        new TableNode{join.table, *opt_col_idx, *opt_prev_col_idx,
                      join.join_flags, TableNode::Children{}});

    auto it_and_inserted =
        node_map.Insert(join.table_name, prev_node->children.back().get());
    if (!it_and_inserted.second) {
      return base::ErrStatus("View has duplicate table name %s",
                             join.table_name);
    }
  }

  // Verify that there all the output columns are well formed.
  {
    base::FlatSet<base::StringView> col_names;
    for (const OutputColumn& col : cols) {
      auto it_and_inserted = col_names.insert(col.col_name);
      if (!it_and_inserted.second) {
        return base::ErrStatus("View has duplicate column %s", col.col_name);
      }

      TableNode** node_it = node_map.Find(col.source_table_name);
      if (!node_it) {
        return base::ErrStatus(
            "View references table %s as source for column %s which does "
            "not exist",
            col.source_table_name, col.col_name);
      }

      const TableNode* node = *node_it;
      if (!node->table->GetColumnIndexByName(col.source_col_name)) {
        return base::ErrStatus(
            "View references column %s in table %s as source for column %s "
            "which does not exist",
            col.source_col_name, col.source_table_name, col.col_name);
      }
    }
  }

  // Build the schema of the output table and a mapping from each output column
  // to the source column which generates it.
  std::vector<SourceColumn> source_col_by_output_idx(cols.size());
  Table::Schema schema;
  for (const OutputColumn& col : cols) {
    const TableNode* node = *node_map.Find(col.source_table_name);
    uint32_t table_col_idx =
        *node->table->GetColumnIndexByName(col.source_col_name);

    const Column& table_col = node->table->GetColumn(table_col_idx);
    PERFETTO_DCHECK(!table_col.IsHidden());

    // TODO(lalitm): if the view specifies the right hand side table as
    // the source for a joined column, we should be able to use the left hand
    // side instead. Add this as a future optimization or detect it and
    // error out.

    base::StringView source_table_name(col.source_table_name);
    schema.columns.emplace_back(Table::Schema::Column{
        col.col_name, table_col.type(),
        source_table_name == root_table_name ? table_col.IsId() : false,
        source_table_name == root_table_name ? table_col.IsSorted() : false,
        table_col.IsHidden(),
        source_table_name == root_table_name ? table_col.IsSetId() : false});

    uint32_t output_idx = static_cast<uint32_t>(schema.columns.size() - 1);
    source_col_by_output_idx[output_idx] = {node, table_col_idx};
  }

  *view = View(std::move(root_node), std::move(source_col_by_output_idx),
               std::move(schema));
  return base::OkStatus();
}

View::View(Table* root_table,
           const char* root_table_name,
           std::initializer_list<JoinTable> joins,
           std::initializer_list<OutputColumn> columns) {
  base::Status status = Create(root_table, root_table_name, std::move(joins),
                               std::move(columns), this);
  if (!status.ok()) {
    PERFETTO_FATAL("Failed building view: %s", status.c_message());
  }
}

Table View::Query(const std::vector<Constraint>& cs,
                  const std::vector<Order>& ob,
                  const BitVector& cols_used) const {
  PERFETTO_DCHECK(cols_used.size() == schema_.columns.size());

  TableNode* root = root_node_.get();

  // Below is the core algorithm which does joining and querying simultaneously.
  // We do this to allow optimizations on which way to order the join and
  // filter based on the join type, constraints, row counters etc.
  //
  // The algorithm is implemented by the |QueryHelper| class for the purposes
  // of sharing a bunch of temporary state between the different stages of the
  // algorithm.

  // The constructor for query helper builds all the temporary state:
  // essentially a copy of the join tree with metadata about which tables are
  // used, which tables remove rows from parents and generates the initial
  // output tables and RowMaps.
  QueryHelper helper(root, source_col_by_output_idx_, cs, cols_used);

  // |FilterAndJoinRecursive| is responsible for filtering all relevant tables
  // which have a constraint necessary for them, materializing any tables
  // participating in the join and computing the "child" table and "parent"
  // RowMap.
  //
  // It does *not* propogate the RowMap downwards: this is done by
  // |ApplyRowMapRecursive|. We don't do this because it would be very
  // inefficient to constantly propogate the RowMap at every level in the middle
  // of a DFS (at its heart, this function is a post-order DFS).
  helper.FilterAndJoinRecursive(root);

  // |ApplyRowMapRecursive| is responsible for recursively propogating the
  // join RowMaps downwards. This is necessary because if you have
  //
  // A JOIN B JOIN C
  //
  // |FilterAndJoinRecursive| will compute the final state of A but only
  // intermediate states for B and C: for B, it will filter out all rows which
  // don't exist in C and for C it will simply leave as-is. The fact that
  // every row in A now has a corresponding row in B and similarily with C
  // is the job of this function.
  //
  // ApplyRowMapRecursive then pushes down the RowMap representing the join A
  // and B and applies that to B. Finally, it selects the B-C RowMap with the
  // A-B RowMap and applies this to C's table.
  helper.ApplyRowMapRecursive(root);

  // |BuildTable| converts the intermediate tables from the above and generates
  // a cohesive table matching the schema of this view. Any "not used" columns
  // are simply replaced with dummy columns who cannot be queried which saves
  // the cost of doing unnecessary joins.
  Table filtered =
      helper.BuildTable(root, schema_, source_col_by_output_idx_, cols_used);

  // The final step is simply to sort the table resulting from filtering.
  //
  // TODO(lalitm): we could be more efficient about this and sort the source
  // tables *before* we join. However, given sorts are relatively rare, we don't
  // do this yet.
  return filtered.Sort(ob);
}

View::QueryHelper::QueryHelper(
    TableNode* root_node,
    const std::vector<SourceColumn>& source_col_by_output_idx,
    const std::vector<Constraint>& cs,
    const BitVector& cols_used)
    : state_(BuildNodeStateMap(root_node,
                               source_col_by_output_idx,
                               cs,
                               cols_used)) {}

void View::QueryHelper::FilterAndJoinRecursive(TableNode* node) {
  NodeState& state = *state_.Find(node);

  // TODO(lalitm): instead of computing the left table straight away here, we
  // could more intelligently figure out whether doing the join first is more
  // efficient.
  state.output = state.output.Filter(state.cs);

  const Table& left_table = state.output;
  RowMap left_rm(0, left_table.row_count());
  for (const auto& child : node->children) {
    NodeState* child_state = state_.Find(child.get());

    // If we have no rows, just bail out to minimize work done.
    if (left_rm.empty())
      break;

    // If the table is not used and doesn't remove any rows in the parent, we
    // can just rely on the default RowMap.
    if (!child_state->is_used && !child_state->removes_parent_rows)
      break;

    // Recurse on the child table so we now the contents of the right table
    // before we filter any further.
    FilterAndJoinRecursive(child.get());

    // If the right table is empty, the left table cannot possibly join
    // without removing rows.
    const Table& right_table = child_state->output;
    if (right_table.row_count() == 0) {
      left_rm = RowMap();
      break;
    }

    const auto& left_col = *TypedColumn<BaseId>::FromColumn(
        &state.output.GetColumn(*child->parent_join_col_idx));
    const auto& right_col = *IdColumn<BaseId>::FromColumn(
        &child_state->output.GetColumn(*child->join_col_idx));

    // The core join loop. This function iterates through every row in
    // the left table and figures out whether to keep it if the row
    // also exists in the right table. While doing this, it also figures
    // out the row number in the right table for every row in the left table.
    std::vector<uint32_t> right_rm_iv;
    right_rm_iv.reserve(left_rm.size());
    left_col.overlay().FilterInto(&left_rm, [&](uint32_t idx) {
      // Check if the right table has the value from the left table.
      std::optional<uint32_t> opt_idx =
          right_col.IndexOf(left_col.GetAtIdx(idx));

      // If it doesn't, return false indicating that this row should be
      // removed from the left table.
      if (!opt_idx)
        return false;

      // If the row does exist, then keep track of the index of the row
      // for applying to the right table and return true to also keep this
      // row in the left table.
      right_rm_iv.emplace_back(*opt_idx);
      return true;
    });
    child_state->parent_join_rm = RowMap(std::move(right_rm_iv));
  }
  state.output = state.output.Apply(std::move(left_rm));
}

void View::QueryHelper::ApplyRowMapRecursive(TableNode* node, RowMap rm) {
  NodeState& state = *state_.Find(node);
  for (const auto& child : node->children) {
    const NodeState& child_state = *state_.Find(child.get());
    // If the child table is not used, then we don't need to recurse any
    // further.
    if (!child_state.is_used)
      break;
    ApplyRowMapRecursive(child.get(),
                         child_state.parent_join_rm.SelectRows(rm));
  }
  state.output = state.output.Apply(std::move(rm));
}

View::QueryHelper::NodeStateMap View::QueryHelper::BuildNodeStateMap(
    TableNode* root_node,
    const std::vector<SourceColumn>& source_col_by_output_idx,
    const std::vector<Constraint>& cs,
    const BitVector& cols_used) {
  // Populate the map contains all the nodes in the tree.
  base::FlatHashMap<const TableNode*, QueryHelper::NodeState> node_state;
  PostOrderDfs(root_node, [&node_state](TableNode* node) {
    node_state.Insert(node,
                      QueryHelper::NodeState{
                          {}, false, false, node->table->Copy(), RowMap()});
  });

  // For each constraint, add the translated constraint to the relevant table's
  // constraint set.
  for (const Constraint& c : cs) {
    const auto& source_col = source_col_by_output_idx[c.col_idx];
    auto& metadata = *node_state.Find(source_col.first);
    metadata.cs.emplace_back(Constraint{source_col.second, c.op, c.value});
  }

  // For each used column, mark the associated table as being used.
  for (auto it = cols_used.IterateSetBits(); it; it.Next()) {
    const auto& source = source_col_by_output_idx[it.index()];
    node_state.Find(source.first)->is_used = true;
  }

  // For each node, figure out whether ti will cause parent rows
  // to be removed.
  for (auto it = node_state.GetIterator(); it; ++it) {
    // The below logic doesn't make sense on the root node.
    if (it.key() == root_node)
      continue;

    // A join will retain (i.e. *not* remove parent rows) if one of the
    // following is true:
    //  a) the child (right-side of join) table contains every id
    //     which could exist in the parent (left-side) table.
    // TODO(lalitm): add more conditions here.
    bool join_retains_parent_rows =
        (it.key()->join_flags & JoinFlag::kIdAlwaysPresent) != 0;

    // However, if this table has constraints, then we could always remove the
    // parents rows even if the join would normally retain all rows.
    it.value().removes_parent_rows =
        !it.value().cs.empty() || !join_retains_parent_rows;
  }

  // Do a DFS on the node tree and propogate up the is_used and
  // removes_parent_rows boolean. In other words, if a table is used by SQLite,
  // every ancestor must also be used as we need to join with every table on the
  // path between the root and used table. Similarily, if a table removes parent
  // rows, then it does this recursively upwards.
  PostOrderDfs(root_node, [&node_state](TableNode* node) {
    NodeState& state = *node_state.Find(node);
    for (const auto& child : node->children) {
      const NodeState& child_metadata = *node_state.Find(child.get());
      state.is_used |= child_metadata.is_used;
      state.removes_parent_rows |= child_metadata.removes_parent_rows;
    }
  });

  return node_state;
}

Table View::QueryHelper::BuildTable(
    TableNode* root,
    const Table::Schema& schema,
    const std::vector<SourceColumn>& source_col_by_output_idx,
    const BitVector& cols_used) {
  NodeState& root_state = *state_.Find(root);

  Table output(root->table->string_pool_);
  output.row_count_ = root_state.output.row_count();
  output.overlays_.emplace_back(ColumnStorageOverlay(output.row_count_));

  std::map<std::pair<const TableNode*, uint32_t>, uint32_t> cached_rm;
  for (auto it = cols_used.IterateAllBits(); it; it.Next()) {
    const char* col_name = schema.columns[it.index()].name.c_str();
    if (!it.IsSet()) {
      output.columns_.emplace_back(
          Column::DummyColumn(col_name, &output, it.index()));
      continue;
    }

    const auto& source_col = source_col_by_output_idx[it.index()];

    Table& node_table = state_.Find(source_col.first)->output;
    const Column& table_col = node_table.GetColumn(source_col.second);

    auto it_and_inserted = cached_rm.emplace(
        std::make_pair(source_col.first, table_col.overlay_index()),
        static_cast<uint32_t>(output.overlays_.size()));
    if (it_and_inserted.second) {
      output.overlays_.emplace_back(
          std::move(node_table.overlays_[table_col.overlay_index()]));
    }

    uint32_t rm_idx = it_and_inserted.first->second;
    output.columns_.emplace_back(
        Column(table_col, &output, it.index(), rm_idx, col_name));
  }
  return output;
}

uint32_t View::GetColumnCount() const {
  return static_cast<uint32_t>(schema_.columns.size());
}

uint32_t View::EstimateRowCount() const {
  uint32_t count = 0;
  PostOrderDfs(root_node_.get(), [&count](TableNode* node) {
    count = std::max(node->table->row_count(), count);
  });
  return count;
}

}  // namespace trace_processor
}  // namespace perfetto
