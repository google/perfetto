// Copyright (C) 2026 The Android Open Source Project
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

import m from 'mithril';

import {displaySize} from '../widgets/flamegraph';
import type {FlamegraphNode, FlamegraphQueryData} from '../widgets/flamegraph';
import {DataGrid} from './widgets/datagrid/datagrid';
import {InMemoryDataSource} from './widgets/datagrid/in_memory_data_source';
import type {
  DataSource,
  DataSourceModel,
  DataSourceRows,
} from './widgets/datagrid/data_source';
import type {IdBasedTree} from './widgets/datagrid/model';
import type {Row, SqlValue} from '../trace_processor/query_result';

const DEFAULT_TREE: IdBasedTree = {
  idField: 'id',
  parentIdField: 'parentId',
  treeColumn: 'name',
  expandedIds: new Set<bigint>(),
};

export interface FlamegraphTableAttrs {
  // Any DataGrid source that implements tree mode: FlamegraphTreeDataSource
  // for already-computed in-memory data, or a SQL source for large trees
  // where each level is fetched on expansion.
  readonly source: DataSource;
  readonly unit: string;
  readonly tree: IdBasedTree | undefined;
  readonly onTreeChanged: (tree: IdBasedTree | undefined) => void;
}

// Tree-table view over a flamegraph: one expandable row per node, with
// self/total values and share of the root total. Uses the DataGrid's id-based
// tree mode, so only expanded levels are produced by the source and only
// on-screen rows hit the DOM.
export class FlamegraphTable implements m.ClassComponent<FlamegraphTableAttrs> {
  view({attrs}: m.CVnode<FlamegraphTableAttrs>): m.Children {
    const {unit} = attrs;
    const fmtValue = (value: SqlValue) =>
      typeof value === 'number' ? displaySize(value, unit) : '';
    return m(DataGrid, {
      className: 'pf-flamegraph-table__grid',
      fillHeight: true,
      schema: {
        name: {title: 'Name', columnType: 'text'},
        total: {
          title: 'Total',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        self: {
          title: 'Self',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        percent: {
          title: '% of total',
          columnType: 'quantitative',
          cellRenderer: (value: SqlValue) =>
            typeof value === 'number' ? `${value.toFixed(1)}%` : '',
        },
      },
      data: attrs.source,
      initialColumns: [
        {id: 'name', field: 'name'},
        {id: 'total', field: 'total', sort: 'DESC'},
        {id: 'self', field: 'self'},
        {id: 'percent', field: 'percent'},
      ],
      tree: attrs.tree ?? DEFAULT_TREE,
      onTreeChanged: attrs.onTreeChanged,
    });
  }
}

// In-memory DataGrid source implementing tree mode over flamegraph nodes.
// Children are indexed by parent id once per data set; each useRows() call
// walks only the expanded portion of the tree, sorting siblings by the
// requested column (total, descending, by default).
export class FlamegraphTreeDataSource extends InMemoryDataSource {
  private readonly children = new Map<number, FlamegraphNode[]>();
  private readonly roots: FlamegraphNode[] = [];
  private readonly total: number;

  constructor(data: FlamegraphQueryData) {
    super([]);
    this.total = data.allRootsCumulativeValue;
    const ids = new Set(data.nodes.map((n) => n.id));
    for (const n of data.nodes) {
      if (ids.has(n.parentId)) {
        let siblings = this.children.get(n.parentId);
        if (siblings === undefined) {
          siblings = [];
          this.children.set(n.parentId, siblings);
        }
        siblings.push(n);
      } else {
        this.roots.push(n);
      }
    }
  }

  useRows(model: DataSourceModel): DataSourceRows {
    if (model.mode !== 'tree') {
      return super.useRows(model);
    }
    const rows = this.visibleRows(model);
    return {rows, totalRows: rows.length, isPending: false};
  }

  exportData(model: DataSourceModel): Promise<readonly Row[]> {
    if (model.mode !== 'tree') {
      return super.exportData(model);
    }
    return Promise.resolve(this.visibleRows(model));
  }

  private visibleRows(model: DataSourceModel & {mode: 'tree'}): readonly Row[] {
    const {expandedIds, collapsedIds} = model.tree;
    const isExpanded = (id: number) =>
      collapsedIds !== undefined
        ? !collapsedIds.has(BigInt(id))
        : (expandedIds?.has(BigInt(id)) ?? false);
    const cmp = this.comparator(model.sort);
    const rows: Row[] = [];
    const visit = (nodes: FlamegraphNode[], depth: number) => {
      for (const n of [...nodes].sort(cmp)) {
        const children = this.children.get(n.id);
        rows.push({
          id: n.id,
          parentId: n.parentId,
          name: n.name,
          total: n.cumulativeValue,
          self: n.selfValue,
          percent:
            this.total === 0 ? 0 : (n.cumulativeValue / this.total) * 100,
          __id: n.id,
          __depth: depth,
          __has_children: children === undefined ? 0 : 1,
        });
        if (children !== undefined && isExpanded(n.id)) {
          visit(children, depth + 1);
        }
      }
    };
    visit(this.roots, 0);
    return rows;
  }

  private comparator(
    sort: {alias: string; direction: 'ASC' | 'DESC'} | undefined,
  ): (a: FlamegraphNode, b: FlamegraphNode) => number {
    const dir = sort?.direction === 'ASC' ? 1 : -1;
    switch (sort?.alias) {
      case 'name':
        return (a, b) => dir * a.name.localeCompare(b.name);
      case 'self':
        return (a, b) => dir * (a.selfValue - b.selfValue);
      default: // 'total', 'percent' and the initial (unsorted) state.
        return (a, b) => dir * (a.cumulativeValue - b.cumulativeValue);
    }
  }
}
