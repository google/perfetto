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
 * Uses string serialization internally for efficient lookup while preserving
 * the original SqlValue types for SQL generation.
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

// Group expansion state for multi-level pivots.
// Each path is an array of groupBy values from level 0 to the expanded level.
// For example, with groupBy: [{field: 'process'}, {field: 'thread'}]:
// - ['processA'] means processA is expanded/collapsed (affecting its threads)

// Whitelist mode: Only groups in expandedGroups are expanded.
// Empty PathSet = all groups collapsed (default when entering multi-level)
export interface PivotWithExpandedGroups extends PivotBase {
  readonly expandedGroups?: PathSet;
}

// Blacklist mode: All groups expanded EXCEPT those in collapsedGroups.
// Empty PathSet = all groups expanded (used by "Expand All")
export interface PivotWithCollapsedGroups extends PivotBase {
  readonly collapsedGroups?: PathSet;
}

export type Pivot =
  | PivotWithExpandedGroups
  | PivotWithCollapsedGroups
  | PivotBase;

export interface Model {
  readonly columns: readonly Column[];
  readonly filters: readonly Filter[];

  // When pivot mode is enabled, columns are ignored.
  // Filters are treated as pre-aggregate filters.
  // TODO(stevegolton): Add post-aggregate (HAVING) filters.
  readonly pivot?: Pivot;
}
