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

import {AsyncLimiter} from '../../../../base/async_limiter';
import {Trace} from '../../../../public/trace';
import {Row} from '../../../../trace_processor/query_result';
import {Filters} from '../legacy_table/filters';
import {buildSqlQuery} from '../legacy_table/query_builder';
import {SimpleColumn} from '../legacy_table/simple_column';
import {
  SqlColumn,
  sqlColumnId,
  SqlExpression,
} from '../legacy_table/sql_column';
import {LegacyTableColumn} from '../legacy_table/table_column';
import {SqlTableDescription} from '../legacy_table/table_description';
import {moveArrayItem} from '../../../../base/array_utils';
import {assertExists} from '../../../../base/logging';
import {SortDirection} from '../../../../base/comparison_utils';
import {Aggregation} from './aggregations';
import {PivotTreeNode} from './pivot_tree_node';
import {aggregationId, pivotId} from './ids';

// Pivot and aggregation ids are human-readable, but are not valid SQLite identifiers,
// so we need to generate valid aliases for them. We map the values back to be keyed
// by the ids as soon as we get the data back from the trace processor.
function pivotSqliteAlias(p: LegacyTableColumn): string {
  return pivotId(p).replace(/[^a-zA-Z0-9_]/g, '__');
}

function aggregationSqliteAlias(a: Aggregation): string {
  return `__${sqlColumnId(a.column.primaryColumn()).replace(/[^a-zA-Z0-9_]/g, '__')}__${a.op}`;
}

interface RequestedData {
  columns: Set<string>;
  query: string;
  result?: {
    rows: Row[];
    tree: PivotTreeNode;
  };
  error?: string;
}

export interface PivotTableStateArgs {
  trace: Trace;
  table: SqlTableDescription;
  pivots: LegacyTableColumn[];
  aggregations?: Aggregation[];
  filters?: Filters;
}

export type SortOrder = {
  type: 'pivot' | 'aggregation';
  id: string;
  direction: SortDirection;
}[];

// State for a pivot table widget.
// Serves as the source-of-truth for: pivots, aggregations, and which parts of the tree are expanded.
// Has a reference to a shared `Filters` object and listens for updates.
// Responsible for generating a query to fetch the data as needed when any of the above change.
export class PivotTableState {
  public readonly table: SqlTableDescription;
  public readonly trace: Trace;
  public readonly filters: Filters;

  private readonly pivots: LegacyTableColumn[] = [];
  private readonly aggregations: Aggregation[] = [];
  private orderBy: SortOrder;

  private limiter: AsyncLimiter = new AsyncLimiter();

  private data: RequestedData;

  constructor(private readonly args: PivotTableStateArgs) {
    this.table = args.table;
    this.trace = args.trace;

    this.pivots = [...args.pivots];
    this.aggregations =
      args.aggregations !== undefined ? [...args.aggregations] : [];
    const count: Aggregation = {
      op: 'count',
      column: new SimpleColumn(new SqlExpression(() => '1', [])),
    };
    this.aggregations.push(count);

    this.orderBy = [
      {type: 'aggregation', id: aggregationId(count), direction: 'DESC'},
    ];

    this.filters = args?.filters ?? new Filters();
    this.filters.addObserver(() => this.reload());

    this.data = {
      columns: new Set(),
      query: '',
    };
    this.reload();
  }

  public getData(): PivotTreeNode | undefined {
    return this.data.result?.tree;
  }

  public getPivots(): ReadonlyArray<LegacyTableColumn> {
    return this.pivots;
  }

  public getAggregations(): ReadonlyArray<Aggregation> {
    return this.aggregations;
  }

  public addPivot(pivot: LegacyTableColumn, index: number) {
    this.pivots.splice(index + 1, 0, pivot);
    this.reload();
  }

  public addAggregation(agg: Aggregation, index: number) {
    this.aggregations.splice(index + 1, 0, agg);
    this.reload();
  }

  public removePivot(index: number) {
    this.sortByPivot(this.pivots[index], undefined);
    this.pivots.splice(index, 1);
    this.reload();
  }

  public removeAggregation(index: number) {
    this.sortByAggregation(this.aggregations[index], undefined);
    this.aggregations.splice(index, 1);
    this.reload();
  }

  public movePivot(from: number, to: number) {
    moveArrayItem(this.pivots, from, to);
    this.reload();
  }

  public moveAggregation(from: number, to: number) {
    moveArrayItem(this.aggregations, from, to);
    this.reload();
  }

  public replaceAggregation(index: number, agg: Aggregation) {
    this.aggregations[index] = agg;
    this.reload();
  }

  public sortByPivot(
    pivot: LegacyTableColumn,
    direction: SortDirection | undefined,
  ) {
    const id = pivotId(pivot);
    // Remove any existing sort by this pivot.
    this.orderBy = this.orderBy.filter(
      (c) => !(c.type === 'pivot' && c.id === id),
    );
    if (direction === undefined) return;
    this.orderBy.unshift({
      type: 'pivot',
      id,
      direction,
    });
    this.data.result?.tree.sort(this.orderBy);
  }

  public sortByAggregation(
    agg: Aggregation,
    direction: SortDirection | undefined,
  ) {
    const id = aggregationId(agg);
    // Remove any existing sort by this aggregation.
    this.orderBy = this.orderBy.filter(
      (c) => !(c.type === 'aggregation' && c.id === id),
    );
    if (direction === undefined) return;
    this.orderBy.unshift({
      type: 'aggregation',
      id,
      direction,
    });
    this.data.result?.tree.sort(this.orderBy);
  }

  public isSortedByPivot(pivot: LegacyTableColumn): SortDirection | undefined {
    if (this.orderBy.length === 0) return undefined;
    const id = pivotId(pivot);
    const head = this.orderBy[0];
    if (head.type === 'pivot' && head.id === id) return head.direction;
    return undefined;
  }

  public isSortedByAggregation(agg: Aggregation): SortDirection | undefined {
    if (this.orderBy.length === 0) return undefined;
    const id = aggregationId(agg);
    const head = this.orderBy[0];
    if (head.type === 'aggregation' && head.id === id) return head.direction;
    return undefined;
  }

  private async reload() {
    this.data.result = undefined;
    this.data.error = undefined;

    this.limiter.schedule(async () => {
      const {query, aliasToIds} = this.buildQuery();

      // TODO(b:395565690): Consider not refetching the data if we already have it.
      this.data = {
        columns: new Set(aliasToIds.values()),
        query,
      };
      const {rows, error} = await this.loadData(query, aliasToIds);
      this.data.error = error;
      if (error === undefined) {
        const tree = PivotTreeNode.buildTree(rows, this);
        tree.sort(this.orderBy);
        this.data.result = {
          rows,
          tree,
        };
      }
    });
  }

  // Generate SQL query to fetch the necessary data.
  // We group by all pivots and apply all aggregations.
  // As ids are not valid sqlite identifiers, we also remember the mapping from alias to id.
  private buildQuery(): {query: string; aliasToIds: Map<string, string>} {
    const columns: {[key: string]: SqlColumn} = {};
    const aliasToIds = new Map<string, string>();
    const groupBy: SqlColumn[] = [];
    for (const pivot of this.pivots) {
      const alias = pivotSqliteAlias(pivot);
      columns[alias] = pivot.primaryColumn();
      aliasToIds.set(alias, pivotId(pivot));
      groupBy.push(pivot.primaryColumn());
    }

    for (const agg of this.aggregations) {
      const alias = aggregationSqliteAlias(agg);
      columns[alias] = new SqlExpression(
        (cols) => `${agg.op}(${cols[0]})`,
        [agg.column.primaryColumn()],
      );
      aliasToIds.set(alias, aggregationId(agg));
    }
    const query = buildSqlQuery({
      table: this.args.table.name,
      columns,
      groupBy,
      filters: this.filters.get(),
    });
    return {
      query,
      aliasToIds,
    };
  }

  // Fetch the data from the trace processor for the given query.
  // To simplify the rest of the code, which uses pivotId / aggregationId as the primary identifiers,
  // we map the data back from the sqlite alises to these ids before returning the data.
  private async loadData(
    query: string,
    aliasToIds: Map<string, string>,
  ): Promise<{
    rows: Row[];
    error?: string;
  }> {
    const res = await this.args.trace.engine.query(query);
    if (res.error() !== undefined) {
      return {rows: [], error: res.error()};
    }
    const rows: Row[] = [];
    for (const it = res.iter({}); it.valid(); it.next()) {
      const row: Row = {};
      for (const column of res.columns()) {
        row[assertExists(aliasToIds.get(column))] = it.get(column);
      }
      rows.push(row);
    }
    return {rows};
  }
}
