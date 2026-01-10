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

import {Row, SqlValue} from '../../../trace_processor/query_result';

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

export interface Pivot {
  // List of fields to group by - supports both new GroupByColumn[]
  readonly groupBy: readonly GroupByColumn[];

  // List of aggregate column definitions.
  readonly aggregates?: readonly AggregateColumn[];

  // When set, shows raw rows filtered by these groupBy column values.
  // This allows drilling down into a specific pivot group to see the
  // underlying data. The keys are the groupBy column names.
  readonly drillDown?: Row;

  // When there are multiple groupBy columns, this controls which parent groups
  // are expanded to show their children. Each path is an array of groupBy values
  // from level 0 to the expanded level.
  // For example, with groupBy: [{field: 'process'}, {field: 'thread'}]:
  // - ['processA'] means processA is expanded (showing its threads)
  // - ['processA', 'threadX'] means threadX under processA is expanded
  readonly expandedGroups?: PathSet;
}

export interface Model {
  readonly columns: readonly Column[];
  readonly filters: readonly Filter[];

  // When pivot mode is enabled, columns are ignored.
  // Filters are treated as pre-aggregate filters.
  // TODO(stevegolton): Add post-aggregate (HAVING) filters.
  readonly pivot?: Pivot;
}
