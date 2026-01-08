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
import {areFiltersEqual, Filter, Filters} from '../table/filters';
import {buildSqlQuery} from '../table/query_builder';
import {SimpleColumn} from '../table/simple_column';
import {SqlColumn, sqlColumnId, SqlExpression} from '../table/sql_column';
import {TableColumn} from '../table/table_column';
import {SqlTableDescription} from '../table/table_description';
import {moveArrayItem} from '../../../../base/array_utils';
import {assertExists} from '../../../../base/logging';
import {SortDirection} from '../../../../base/comparison_utils';
import {Aggregation, expandAggregations} from './aggregations';
import {PivotTreeNode} from './pivot_tree_node';
import {aggregationId, pivotId} from './ids';
import {uuidv4} from '../../../../base/uuid';

// Pivot and aggregation ids are human-readable, but are not valid SQLite identifiers,
// so we need to generate valid aliases for them. We map the values back to be keyed
// by the ids as soon as we get the data back from the trace processor.
function pivotSqliteAlias(p: TableColumn): string {
  return pivotId(p).replace(/[^a-zA-Z0-9_]/g, '__');
}

function aggregationSqliteAlias(a: Aggregation): string {
  return `__${sqlColumnId(a.column.column).replace(/[^a-zA-Z0-9_]/g, '__')}__${a.op}`;
}

interface RequestedData {
  columnIds: Set<string>;
  filters: Filter[];
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
  pivots: TableColumn[];
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
  public readonly uuid: string;

  private readonly pivots: TableColumn[] = [];
  private readonly aggregations: Aggregation[] = [];
  private orderBy: SortOrder;

  private limiter: AsyncLimiter = new AsyncLimiter();

  private data: RequestedData;
  // Used to keep track of the tree before a reload, so we can keep the same nodes expanded.
  private oldTree?: PivotTreeNode;

  constructor(private readonly args: PivotTableStateArgs) {
    this.table = args.table;
    this.trace = args.trace;
    this.uuid = uuidv4();

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
      columnIds: new Set(),
      filters: [],
      query: '',
    };
    this.reload();
  }

  public getData(): PivotTreeNode | undefined {
    return this.data.result?.tree;
  }

  public getPivots(): ReadonlyArray<TableColumn> {
    return this.pivots;
  }

  public getAggregations(): ReadonlyArray<Aggregation> {
    return this.aggregations;
  }

  public addPivot(pivot: TableColumn, index: number) {
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

  public sortByPivot(pivot: TableColumn, direction: SortDirection | undefined) {
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

  public clearPivotSort(pivot: TableColumn) {
    const id = pivotId(pivot);
    this.orderBy = this.orderBy.filter(
      (c) => !(c.type === 'pivot' && c.id === id),
    );
    this.data.result?.tree.sort(this.orderBy);
  }

  public clearAggregationSort(agg: Aggregation) {
    const id = aggregationId(agg);
    this.orderBy = this.orderBy.filter(
      (c) => !(c.type === 'aggregation' && c.id === id),
    );
    this.data.result?.tree.sort(this.orderBy);
  }

  public isSortedByPivot(pivot: TableColumn): SortDirection | undefined {
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
    this.oldTree = this.data.result?.tree ?? this.oldTree;

    this.limiter.schedule(async () => {
      const {query, columnIds, aliasToIds} = this.buildQuery();

      // Check if we already have all of the columns (and the filters are the same): in that case
      // we don't need to reload.
      // Note that comparing the queries directly is too sensitive for us: e.g. we don't care about
      // the column ordering, as well as having extra aggregations.
      const needsReload =
        this.data.error !== undefined ||
        !areFiltersEqual(this.filters.get(), this.data.filters) ||
        ![...columnIds].every((id) => this.data.columnIds.has(id));
      // If we don't need to reload, we can keep the old rows.
      let rows = needsReload ? undefined : this.data.result?.rows;

      this.data = {
        columnIds: new Set(aliasToIds.values()),
        filters: [...this.filters.get()],
        query,
      };
      // If we need to reload, fetch the data from the trace processor.
      if (rows === undefined) {
        const queryResult = await this.loadData(query, aliasToIds);
        this.data.error = queryResult.error;
        rows = queryResult.rows;
      }
      if (this.data.error === undefined) {
        // Build the pivot tree from the rows.
        const tree = PivotTreeNode.buildTree(rows, {
          pivots: this.getPivots(),
          aggregations: this.getAggregations(),
        });

        // If we have an old tree, copy the expanded state from it.
        tree.copyExpandedState(this.oldTree);
        this.oldTree = undefined;

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
  private buildQuery(): {
    query: string;
    columnIds: Set<string>;
    aliasToIds: Map<string, string>;
  } {
    const columns: {[key: string]: SqlColumn} = {};
    const columnIds = new Set<string>();
    const aliasToIds = new Map<string, string>();
    const groupBy: SqlColumn[] = [];
    for (const pivot of this.pivots) {
      const alias = pivotSqliteAlias(pivot);
      columns[alias] = pivot.column;
      columnIds.add(pivotId(pivot));
      aliasToIds.set(alias, pivotId(pivot));
      groupBy.push(pivot.column);
    }

    // Expand non-assocative aggregations (average) into basic associative aggregations which
    // can be pushed down to SQL.
    for (const agg of expandAggregations(this.aggregations)) {
      const alias = aggregationSqliteAlias(agg);
      columns[alias] = new SqlExpression(
        (cols) => `${agg.op}(${cols[0]})`,
        [agg.column.column],
      );
      columnIds.add(aggregationId(agg));
      aliasToIds.set(alias, aggregationId(agg));
    }
    const query = buildSqlQuery({
      table: this.args.table.name,
      columns,
      groupBy,
      filters: this.filters.get(),
    });
    const importStatement = (this.table.imports ?? [])
      .map((i) => `INCLUDE PERFETTO MODULE ${i};\n`)
      .join('');
    return {
      query: `${importStatement}${query}`,
      columnIds,
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
