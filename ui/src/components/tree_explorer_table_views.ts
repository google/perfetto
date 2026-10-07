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

// Table views over tree explorer data: an expandable call tree and a flat
// per-function table, both rendered with DataGrid over the nodes the
// flamegraph canvas already holds — nothing extra is fetched.

import m from 'mithril';
import {assertUnreachable} from '../base/assert';
import {
  formatAsTSV,
  formatAsJSON,
  formatAsMarkdown,
} from '../base/export_formatters';
import type {ExportFormat} from '../widgets/export_button';
import {
  displaySize,
  getUnitDisplayName,
  type TreeExplorerData,
  type TreeExplorerMetric,
  type TreeExplorerNode,
} from '../widgets/tree_explorer';
import {DataGrid} from './widgets/datagrid/datagrid';
import {InMemoryDataSource} from './widgets/datagrid/in_memory_data_source';
import type {
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

export interface TreeExplorerTreeViewAttrs {
  readonly data: TreeExplorerData;
  readonly unit: string;
}

// Expandable call tree: one row per node with self and cumulative values and
// the share of the root total. DataGrid id-based tree mode keeps expansion
// O(visible rows) and the DOM virtualised. Direction (top-down vs bottom-up)
// is a property of the fetched tree, not of this widget.
export class TreeExplorerTreeView implements m.ClassComponent<TreeExplorerTreeViewAttrs> {
  private source?: TreeExplorerTreeDataSource;
  private sourceData?: TreeExplorerData;
  private tree?: IdBasedTree;

  view({attrs}: m.CVnode<TreeExplorerTreeViewAttrs>): m.Children {
    if (this.source === undefined || this.sourceData !== attrs.data) {
      this.sourceData = attrs.data;
      this.source = new TreeExplorerTreeDataSource(attrs.data);
      this.tree = undefined;
    }
    const fmtValue = (value: SqlValue) =>
      typeof value === 'number' ? displaySize(value, attrs.unit) : '';
    return m(DataGrid, {
      className: 'pf-tree-explorer__grid',
      fillHeight: true,
      // The tree explorer's own filter bar is the filtering surface;
      // row-level grid filters have no meaning on a tree walk.
      disableFilterControls: true,
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
          cellRenderer: fmtPercent,
        },
      },
      data: this.source,
      initialColumns: [
        {id: 'name', field: 'name'},
        {id: 'total', field: 'total', sort: 'DESC'},
        {id: 'self', field: 'self'},
        {id: 'percent', field: 'percent'},
      ],
      tree: this.tree ?? DEFAULT_TREE,
      onTreeChanged: (tree) => {
        this.tree = tree;
      },
    });
  }
}

// Tree-mode DataGrid source over resident tree explorer nodes. Children are
// indexed once; useRows() walks only the expanded rows, sorting siblings by
// the requested column (total descending by default).
class TreeExplorerTreeDataSource extends InMemoryDataSource {
  private readonly children = new Map<number, TreeExplorerNode[]>();
  private readonly roots: TreeExplorerNode[] = [];
  private readonly total: number;

  constructor(data: TreeExplorerData) {
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
    const visit = (nodes: TreeExplorerNode[], depth: number) => {
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
  ): (a: TreeExplorerNode, b: TreeExplorerNode) => number {
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

export interface TreeExplorerFlatFunction {
  readonly name: string;
  readonly self: number;
  readonly total: number;
}

// Aggregates the tree per function name, pprof-style: `self` sums a
// function's self value over every node with that name; `total` sums its
// cumulative value counting only the topmost occurrence on each path, so
// recursion (A -> B -> A) does not double-count A. Only the callee direction
// (depth > 0) is aggregated: in PIVOT view the negative-depth caller nodes
// re-attribute the same samples.
export function computeFlatFunctions(
  data: TreeExplorerData,
): TreeExplorerFlatFunction[] {
  const nodes = data.nodes.filter((n) => n.depth > 0);
  const ids = new Set(nodes.map((n) => n.id));
  const children = new Map<number, TreeExplorerNode[]>();
  const roots: TreeExplorerNode[] = [];
  for (const n of nodes) {
    if (ids.has(n.parentId)) {
      let siblings = children.get(n.parentId);
      if (siblings === undefined) {
        siblings = [];
        children.set(n.parentId, siblings);
      }
      siblings.push(n);
    } else {
      roots.push(n);
    }
  }

  const functions = new Map<string, {self: number; total: number}>();
  // Number of times each name occurs on the current root-to-node path.
  // Explicit stack with exit markers: call stacks can be deep.
  const onPath = new Map<string, number>();
  for (const root of roots) {
    const stack: Array<{node: TreeExplorerNode; exit: boolean}> = [
      {node: root, exit: false},
    ];
    while (stack.length > 0) {
      const {node, exit} = stack.pop()!;
      const name = node.name;
      if (exit) {
        onPath.set(name, (onPath.get(name) ?? 1) - 1);
        continue;
      }
      let fn = functions.get(name);
      if (fn === undefined) {
        fn = {self: 0, total: 0};
        functions.set(name, fn);
      }
      fn.self += node.selfValue;
      const occurrences = onPath.get(name) ?? 0;
      if (occurrences === 0) {
        fn.total += node.cumulativeValue;
      }
      onPath.set(name, occurrences + 1);
      stack.push({node, exit: true});
      for (const child of children.get(node.id) ?? []) {
        stack.push({node: child, exit: false});
      }
    }
  }
  return [...functions].map(([name, {self, total}]) => ({name, self, total}));
}

export interface TreeExplorerFlatViewAttrs {
  readonly data: TreeExplorerData;
  readonly unit: string;
}

// Flat per-function table (pprof "Top"): Name, Self, Self %, Total, Total %.
export class TreeExplorerFlatView implements m.ClassComponent<TreeExplorerFlatViewAttrs> {
  private rows?: readonly Row[];
  private rowsData?: TreeExplorerData;

  view({attrs}: m.CVnode<TreeExplorerFlatViewAttrs>): m.Children {
    if (this.rows === undefined || this.rowsData !== attrs.data) {
      this.rowsData = attrs.data;
      this.rows = flatFunctionRows(attrs.data);
    }
    const fmtValue = (value: SqlValue) =>
      typeof value === 'number' ? displaySize(value, attrs.unit) : '';
    return m(DataGrid, {
      className: 'pf-tree-explorer__grid',
      fillHeight: true,
      disableFilterControls: true,
      schema: {
        name: {title: 'Name', columnType: 'text'},
        self: {
          title: 'Self',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        selfPercent: {
          title: 'Self %',
          columnType: 'quantitative',
          cellRenderer: fmtPercent,
        },
        total: {
          title: 'Total',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        totalPercent: {
          title: 'Total %',
          columnType: 'quantitative',
          cellRenderer: fmtPercent,
        },
      },
      data: this.rows,
      initialColumns: [
        {id: 'name', field: 'name'},
        {id: 'self', field: 'self', sort: 'DESC'},
        {id: 'selfPercent', field: 'selfPercent'},
        {id: 'total', field: 'total'},
        {id: 'totalPercent', field: 'totalPercent'},
      ],
    });
  }
}

function flatFunctionRows(data: TreeExplorerData): readonly Row[] {
  const total = data.allRootsCumulativeValue;
  const pct = (value: number) => (total === 0 ? 0 : (value / total) * 100);
  return computeFlatFunctions(data).map((fn) => ({
    name: fn.name,
    self: fn.self,
    selfPercent: pct(fn.self),
    total: fn.total,
    totalPercent: pct(fn.total),
  }));
}

function fmtPercent(value: SqlValue): string {
  return typeof value === 'number' ? `${value.toFixed(1)}%` : '';
}

// Export of the flat function table. Values are raw numbers in the metric's
// unit so they can be aggregated in spreadsheets.
export function buildFlatExportString(
  data: TreeExplorerData,
  metric: TreeExplorerMetric,
  format: ExportFormat,
): string {
  const unitDisplay = getUnitDisplayName(metric.unit);
  const columns = ['name', 'self', 'selfPercent', 'total', 'totalPercent'];
  const columnNames: Record<string, string> = {
    name: metric.nameColumnLabel ?? 'Name',
    self: `Self ${metric.name} (${unitDisplay})`,
    selfPercent: 'Self %',
    total: `Total ${metric.name} (${unitDisplay})`,
    totalPercent: 'Total %',
  };
  const total = data.allRootsCumulativeValue;
  const pct = (value: number) =>
    (total === 0 ? 0 : (value / total) * 100).toFixed(2);
  const rows = computeFlatFunctions(data)
    .sort((a, b) => b.self - a.self)
    .map((fn) => ({
      name: fn.name,
      self: fn.self.toString(),
      selfPercent: pct(fn.self),
      total: fn.total.toString(),
      totalPercent: pct(fn.total),
    }));
  switch (format) {
    case 'tsv':
      return formatAsTSV(columns, columnNames, rows);
    case 'json':
      return formatAsJSON(columns, columnNames, rows);
    case 'markdown':
      return formatAsMarkdown(columns, columnNames, rows);
    default:
      assertUnreachable(format);
  }
}
