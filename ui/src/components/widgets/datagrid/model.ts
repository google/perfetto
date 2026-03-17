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

export type AggregateFunction =
  | 'ANY'
  | 'SUM'
  | 'AVG'
  | 'MIN'
  | 'MAX'
  | 'COUNT_DISTINCT';
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

// A path in the pivot tree, represented as an array of group column values.
// For a tree with groupBy [category, name]:
// - Root path: []
// - Category "Foo": ["Foo"]
// - Category "Foo", Name "Bar": ["Foo", "Bar"]
// Paths are stable across configuration changes (unlike internal IDs).
export type GroupPath = readonly SqlValue[];

// Value-based expansion mode: Uses group column values as stable identifiers.
//
// Two mutually exclusive modes:
// 1. Allowlist (expandedGroups): Only specified paths are expanded.
//    Empty array = only root children visible (all collapsed).
// 2. Denylist (collapsedGroups): All nodes expanded EXCEPT those specified.
//    Empty array = all nodes expanded.
//
// If both are set, collapsedGroups takes precedence.
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

  // Allowlist mode: only these group paths are expanded
  readonly expandedGroups?: readonly GroupPath[];
  // Denylist mode: all nodes expanded except these group paths
  readonly collapsedGroups?: readonly GroupPath[];
}

// ID-based tree configuration for displaying hierarchical data using
// id/parent_id columns. Uses recursive CTE for efficient tree traversal with
// sorting. Unlike path-based pivot tree, this expects the data to have explicit
// id and parent_id columns.
//
// Expansion modes (mutually exclusive):
// 1. Allowlist (expandedIds): Only specified node IDs are expanded. Empty set =
//    only root nodes visible (all collapsed).
// 2. Denylist (collapsedIds): All nodes expanded EXCEPT those specified. Empty
//    set = all nodes expanded.
// 3. Neither set: Default to showing only root level (collapsed by default).
//
// If both are set, collapsedIds takes precedence (denylist mode).
export interface IdBasedTree {
  // The field in the datasouce containing the row's unique ID
  readonly idField: string;
  // The field in the datasouce containing the parent row's ID (NULL for root
  // nodes)
  readonly parentIdField: string;
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

  // ID-based tree mode for displaying hierarchical data with id/parent_id columns.
  // Uses recursive CTE for tree traversal with proper sorting within siblings.
  // Supports expand/collapse and renders chevrons on the tree column.
  // Mutually exclusive with pivot mode.
  readonly tree?: IdBasedTree;
}
