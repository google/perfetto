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

import {SqlValue} from '../../../trace_processor/query_result';

export type AggregateFunction = 'ANY' | 'SUM' | 'AVG' | 'MIN' | 'MAX';
export type SortDirection = 'ASC' | 'DESC';
export type GroupDisplay = 'flat' | 'tree';
export const DEFAULT_GROUP_DISPLAY: GroupDisplay = 'flat';

interface ColumnBase {
  // Unique identifier for this column. Allows multiple columns with the same
  // field but different configurations (e.g., different aggregate functions).
  readonly id: string;
  readonly sort?: SortDirection;
}

export interface Column extends ColumnBase {
  readonly field: string;
  readonly aggregate?: AggregateFunction; // Rename to summary
}

export type Filter = {readonly field: string} & FilterOpAndValue;

interface OpFilter {
  readonly op: '=' | '!=' | '<' | '<=' | '>' | '>=' | 'glob' | 'not glob';
  readonly value: SqlValue;
}

interface InFilter {
  readonly op: 'in' | 'not in';
  readonly value: readonly SqlValue[];
}

interface NullFilter {
  readonly op: 'is null' | 'is not null';
}

export type FilterOpAndValue = OpFilter | InFilter | NullFilter;

interface AggregateField extends ColumnBase {
  readonly function: AggregateFunction;
  readonly field: string;
}

interface AggregateFieldCount extends ColumnBase {
  readonly function: 'COUNT';
}

export type AggregateColumn = AggregateField | AggregateFieldCount;

export interface GroupByColumn extends ColumnBase {
  readonly field: string;
}

// ID-based expansion mode: Uses numeric node IDs from __intrinsic_rollup_tree virtual table.
// The virtual table maintains the tree structure and handles ROLLUP-style aggregation.
//
// Two mutually exclusive modes:
// 1. Allowlist (expandedIds): Only specified nodes are expanded.
//    Empty Set = only root children visible (all collapsed).
// 2. Denylist (collapsedIds): All nodes expanded EXCEPT those specified.
//    Empty Set = all nodes expanded.
//
// If both are set, collapsedIds takes precedence.
export interface Pivot {
  // List of fields to group by
  readonly groupBy: readonly GroupByColumn[];

  // List of aggregate column definitions.
  readonly aggregates: readonly AggregateColumn[];

  // When set, shows raw rows filtered by these groupBy column values.
  // This allows drilling down into a specific pivot group to see the
  // underlying data. The keys are the groupBy column names.
  readonly drillDown?: readonly {field: string; value: SqlValue}[];

  // How to display grouped data: 'flat' shows leaf rows only, 'tree' shows
  // hierarchical structure with expand/collapse.
  readonly groupDisplay?: GroupDisplay;

  // Allowlist mode: only these node IDs are expanded
  readonly expandedIds?: ReadonlySet<bigint>;
  // Denylist mode: all nodes expanded except these IDs
  readonly collapsedIds?: ReadonlySet<bigint>;
}

// ID-based tree configuration for displaying hierarchical data using id/parent_id columns.
// Uses __intrinsic_tree virtual table for efficient tree operations.
// Unlike path-based TreeGrouping, this expects the data to have explicit id and parent_id columns.
export interface IdBasedTree {
  // Column containing the row's unique ID
  readonly idColumn: string;
  // Column containing the parent's ID (NULL for root nodes)
  readonly parentIdColumn: string;
  // Column to display as the tree (shows chevrons and indentation)
  // If not specified, the first visible column is used
  readonly treeColumn?: string;
  // Allowlist mode: only these node IDs are expanded
  readonly expandedIds?: ReadonlySet<bigint>;
  // Denylist mode: all nodes expanded except these IDs
  readonly collapsedIds?: ReadonlySet<bigint>;
}
export interface Model {
  readonly columns: readonly Column[];
  readonly filters: readonly Filter[];

  // When pivot mode is enabled, columns are ignored.
  // Filters are treated as pre-aggregate filters.
  // TODO(stevegolton): Add post-aggregate (HAVING) filters.
  readonly pivot?: Pivot;

  // ID-based tree mode using __intrinsic_tree virtual table.
  // Similar to tree mode but uses explicit id/parent_id columns instead of
  // path-based hierarchy. Supports sorting and expand/collapse like pivot mode.
  // Mutually exclusive with pivot and tree modes.
  readonly idBasedTree?: IdBasedTree;
}
