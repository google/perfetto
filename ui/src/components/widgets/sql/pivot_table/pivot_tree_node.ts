// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {range} from '../../../../base/array_utils';
import {assertExists, assertTrue} from '../../../../base/logging';
import {Row} from '../../../../trace_processor/query_result';
import {SqlValue} from '../../../../trace_processor/sql_utils';
import {basicAggregations} from './aggregations';
import {aggregationId, pivotId} from './ids';
import type {PivotTableState, SortOrder} from './pivot_table_state';

// assertExists trips over NULLs, but NULL is a valid SQL value we have to work with.
function assertNotUndefined<T>(value: T | undefined): T {
  if (value === undefined) throw new Error('Value is undefined');
  return value;
}

// A node in the pivot tree.
// Each node represents a partially aggregated values for the first `depth` pivots.
export class PivotTreeNode {
  private readonly state: PivotTableState;

  private readonly parent?: PivotTreeNode;
  // undefined only for the root node.
  // Note: it can be NULL, which can trip over assertExists.
  private readonly pivotValue?: SqlValue;
  // 0 for the root node.
  private readonly depth: number;

  private readonly children: Map<SqlValue, PivotTreeNode>;
  // The aggregated values for the node itself.
  readonly aggregationValues: SqlValue[];
  // The aggregated values for the node and all its descendants.
  private aggregations: SqlValue[];
  collapsed: boolean;

  constructor(args: {
    state: PivotTableState;
    parent?: PivotTreeNode;
    pivotValue?: SqlValue;
  }) {
    this.state = args.state;

    this.parent = args.parent;
    this.pivotValue = args.pivotValue;
    this.depth = this.parent === undefined ? 0 : this.parent.depth + 1;

    this.aggregationValues = range(this.state.getAggregations().length).map(
      () => null,
    );
    this.aggregations = [...this.aggregationValues];
    this.children = new Map();
    this.collapsed = this.depth > 0;
  }

  // The index of the last pivot value in the pivot list.
  getPivotIndex(): number {
    return this.depth - 1;
  }

  // Construct the tree from the given rows.
  // The rows should be indexed by `pivotId` and `aggregationId`.
  static buildTree(rows: Row[], state: PivotTableState): PivotTreeNode {
    const root = new PivotTreeNode({state});
    for (const row of rows) {
      let node = root;
      for (const pivot of state.getPivots()) {
        node = node.getOrCreateChild(row[pivotId(pivot)]);
      }
      for (const [index, agg] of state.getAggregations().entries()) {
        node.aggregationValues[index] = row[aggregationId(agg)];
      }
    }
    root.update();
    return root;
  }

  // Get the value of the pivot at the given index.
  getPivotValue(index: number): SqlValue | undefined {
    // depth of 0 is the root node, so the actual values start
    // with depth 1.
    const targetDepth = index + 1;
    if (targetDepth > this.depth) return undefined;
    if (targetDepth === this.depth) return this.pivotValue;
    return assertExists(this.parent).getPivotValue(index);
  }

  // Get the value of the aggregation at the given index.
  getAggregationValue(index: number): SqlValue {
    return this.aggregations[index];
  }

  // List all of the descendants of this node, respecting `collapsed` state.
  *listDescendants(): Generator<PivotTreeNode> {
    yield this;
    if (this.collapsed) return;
    for (const child of this.children.values()) {
      yield* child.listDescendants();
    }
  }

  // Recursively sort the subtree according to the given order.
  sort(order: SortOrder) {
    if (order.length === 0) return;

    for (const child of this.children.values()) {
      child.sort(order);
    }
    const sorted = [...this.children.values()].sort((lhs, rhs) =>
      PivotTreeNode.compare(lhs, rhs, order),
    );
    this.children.clear();
    for (const child of sorted) {
      this.children.set(assertNotUndefined(child.pivotValue), child);
    }
  }

  private getOrCreateChild(value: SqlValue): PivotTreeNode {
    if (!this.children.has(value)) {
      this.children.set(
        value,
        new PivotTreeNode({
          state: this.state,
          parent: this,
          pivotValue: value,
        }),
      );
    }
    return assertExists(this.children.get(value));
  }

  private update() {
    this.aggregations = [...this.aggregationValues];
    for (const child of this.children.values()) {
      child.update();
      for (const [index, agg] of this.state.getAggregations().entries()) {
        this.aggregations[index] = basicAggregations[agg.op](
          this.aggregations[index],
          child.aggregations[index],
        );
      }
    }
  }

  // Compare two nodes according to the given sort order.
  private static compare(
    lhs: PivotTreeNode,
    rhs: PivotTreeNode,
    order: SortOrder,
  ): number {
    // Note: resolving items in `order` requires a lookup in the state. We can consider
    // optimising this and performing the lookup in `sort` instead.

    // We should only compare siblings.
    assertTrue(
      lhs.state === rhs.state &&
        lhs.depth === rhs.depth &&
        lhs.parent === rhs.parent,
    );

    const compareSqlValues = (lhs: SqlValue, rhs: SqlValue) => {
      if (lhs === rhs) return 0;
      // Nulls can't be compared, but should be considered the smallest value.
      if (lhs === null) return -1;
      if (rhs === null) return 1;
      return lhs < rhs ? -1 : 1;
    };
    for (const {type, id, direction} of order) {
      if (type === 'aggregation') {
        const index = lhs.state
          .getAggregations()
          .findIndex((a) => aggregationId(a) === id);
        // Aggregation with this index should always exist.
        // If this is not the case, we probably failed to remove sorting after
        // hiding a column.
        assertTrue(index !== -1);
        const cmp = compareSqlValues(
          lhs.aggregations[index],
          rhs.aggregations[index],
        );
        if (cmp !== 0) return direction === 'ASC' ? cmp : -cmp;
      } else {
        const index = lhs.state.getPivots().findIndex((p) => pivotId(p) === id);
        // Pivot with this index should always exist.
        // If this is not the case, we probably failed to remove sorting after
        // hiding a column.
        assertTrue(index !== -1);
        // For pivot sorting, we only compare the pivot values at the given depth.
        if (index + 1 === lhs.depth) {
          const cmp = compareSqlValues(
            assertNotUndefined(lhs.pivotValue),
            assertNotUndefined(rhs.pivotValue),
          );
          if (cmp !== 0) return direction === 'ASC' ? cmp : -cmp;
        }
      }
    }
    return 0;
  }
}
