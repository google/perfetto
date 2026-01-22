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

/**
 * A Set-like collection for storing paths of SqlValue arrays.
 * Used for tree grouping expansion state (path-based).
 */
export class PathSet implements Iterable<readonly SqlValue[]> {
  private readonly map = new Map<string, readonly SqlValue[]>();

  constructor(paths?: Iterable<readonly SqlValue[]>) {
    if (paths) {
      for (const path of paths) {
        this.add(path);
      }
    }
  }

  private static toKey(path: readonly SqlValue[]): string {
    return path.map((v) => String(v)).join('\x00');
  }

  add(path: readonly SqlValue[]): this {
    this.map.set(PathSet.toKey(path), path);
    return this;
  }

  has(path: readonly SqlValue[]): boolean {
    return this.map.has(PathSet.toKey(path));
  }

  delete(path: readonly SqlValue[]): boolean {
    return this.map.delete(PathSet.toKey(path));
  }

  get size(): number {
    return this.map.size;
  }

  [Symbol.iterator](): Iterator<readonly SqlValue[]> {
    return this.map.values();
  }

  values(): IterableIterator<readonly SqlValue[]> {
    return this.map.values();
  }
}

export type AggregateFunction = 'ANY' | 'SUM' | 'AVG' | 'MIN' | 'MAX';
export type SortDirection = 'ASC' | 'DESC';

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
  // If set, treat this column as a tree column with hierarchical path values.
  // The column value is expected to contain slash-separated paths like
  // "blink_gc/main/allocated_objects".
  readonly tree?: {
    readonly delimiter?: string; // Default: '/'
  };
}

// Base pivot configuration without expansion state
interface PivotBase {
  // List of fields to group by
  readonly groupBy: readonly GroupByColumn[];

  // List of aggregate column definitions.
  readonly aggregates?: readonly AggregateColumn[];

  // When set, shows raw rows filtered by these groupBy column values.
  // This allows drilling down into a specific pivot group to see the
  // underlying data. The keys are the groupBy column names.
  readonly drillDown?: readonly {field: string; value: SqlValue}[];

  // When true, shows leaf-level rows only (no rollup/summary rows).
  // This displays the data in a flat table format without hierarchical grouping.
  readonly collapsibleGroups?: boolean;
}

// ID-based expansion mode: Uses numeric node IDs from __intrinsic_pivot virtual table.
// The virtual table maintains the tree structure and handles ROLLUP-style aggregation.
//
// Two mutually exclusive modes:
// 1. Whitelist (expandedIds): Only specified nodes are expanded.
//    Empty Set = only root children visible (all collapsed).
// 2. Blacklist (collapsedIds): All nodes expanded EXCEPT those specified.
//    Empty Set = all nodes expanded.
//
// If both are set, collapsedIds takes precedence.
export interface Pivot extends PivotBase {
  // Whitelist mode: only these node IDs are expanded
  readonly expandedIds?: ReadonlySet<bigint>;
  // Blacklist mode: all nodes expanded except these IDs
  readonly collapsedIds?: ReadonlySet<bigint>;
}

// Tree grouping configuration for displaying hierarchical data in flat mode.
// Unlike pivot mode, tree grouping displays raw column values without aggregation.
// It simply filters rows based on expansion state and adds tree UI (indent, chevrons).
interface TreeGroupingBase {
  // The column field containing hierarchical paths (e.g., "blink_gc/main/foo")
  readonly field: string;
  // Path delimiter (default: '/')
  readonly delimiter?: string;
}

// Whitelist mode: Only paths in expandedPaths are expanded.
// Empty PathSet = all collapsed (default)
export interface TreeGroupingWithExpandedPaths extends TreeGroupingBase {
  readonly expandedPaths?: PathSet;
}

// Blacklist mode: All paths are expanded except those in collapsedPaths.
// Empty PathSet = all expanded (used by "Expand All")
export interface TreeGroupingWithCollapsedPaths extends TreeGroupingBase {
  readonly collapsedPaths: PathSet;
}

export type TreeGrouping =
  | TreeGroupingWithExpandedPaths
  | TreeGroupingWithCollapsedPaths
  | TreeGroupingBase;

export interface Model {
  readonly columns: readonly Column[];
  readonly filters: readonly Filter[];

  // When pivot mode is enabled, columns are ignored.
  // Filters are treated as pre-aggregate filters.
  // TODO(stevegolton): Add post-aggregate (HAVING) filters.
  readonly pivot?: Pivot;

  // Tree grouping mode for displaying hierarchical data without aggregation.
  // Mutually exclusive with pivot mode.
  // When set, one column displays as a tree with expand/collapse,
  // and other columns show their raw values.
  readonly tree?: TreeGrouping;
}
