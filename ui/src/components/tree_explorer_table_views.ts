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
  displayChange,
  displaySize,
  exportRelativeChange,
  getUnitDisplayName,
  relativeChange,
  type TreeExplorerData,
  type TreeExplorerDiff,
  type TreeExplorerMetric,
  type TreeExplorerNode,
  type TreeExplorerNodeIconFn,
  type TreeExplorerNodeTooltipRenderer,
} from '../widgets/tree_explorer';
import {DataGrid} from './widgets/datagrid/datagrid';
import {InMemoryDataSource} from './widgets/datagrid/in_memory_data_source';
import type {
  DataSourceModel,
  DataSourceRows,
} from './widgets/datagrid/data_source';
import type {IdBasedTree} from './widgets/datagrid/model';
import type {Row, SqlValue} from '../trace_processor/query_result';
import {diffColors} from '../widgets/flamegraph';
import {
  displayPercentPointDelta,
  roundedSign,
} from '../widgets/flamegraph_diff';
import {Icon} from '../widgets/icon';
import {Tooltip} from '../widgets/tooltip';

const DEFAULT_TREE: IdBasedTree = {
  idField: 'id',
  parentIdField: 'parentId',
  treeColumn: 'name',
  expandedIds: new Set<bigint>(),
};

// Largest growth and shrink of Self Δ across the merged Functions rows,
// matching Functions' pill-colored Self Δ column and legend.
export function computeFlatDiff(
  diff: TreeExplorerDiff,
  functions: ReadonlyArray<TreeExplorerFlatFunction>,
): TreeExplorerDiff {
  let maxGrowth = 0;
  let maxShrink = 0;
  for (const fn of functions) {
    const selfDelta = fn.self - (fn.baselineSelf ?? 0);
    maxGrowth = Math.max(maxGrowth, selfDelta);
    maxShrink = Math.max(maxShrink, -selfDelta);
  }
  return {...diff, maxGrowth, maxShrink};
}

export interface TreeExplorerTreeViewAttrs {
  readonly data: TreeExplorerData;
  readonly unit: string;
  // See TreeExplorerPanelAttrs.
  readonly nodeIcon?: TreeExplorerNodeIconFn;
  readonly renderNodeTooltip?: TreeExplorerNodeTooltipRenderer;
}

// Expandable call tree: one row per node with self and cumulative values and
// the share of the root total. DataGrid id-based tree mode keeps expansion
// O(visible rows) and the DOM virtualised. Direction (top-down vs bottom-up)
// is a property of the fetched tree, not of this widget.
export class TreeExplorerTreeView implements m.ClassComponent<TreeExplorerTreeViewAttrs> {
  private source?: TreeExplorerTreeDataSource;
  private sourceData?: TreeExplorerData;
  private tree?: IdBasedTree;
  private nodesById = new Map<number, TreeExplorerNode>();

  view({attrs}: m.CVnode<TreeExplorerTreeViewAttrs>): m.Children {
    if (this.source === undefined || this.sourceData !== attrs.data) {
      this.sourceData = attrs.data;
      this.source = new TreeExplorerTreeDataSource(attrs.data);
      this.tree = undefined;
      this.nodesById = new Map(attrs.data.nodes.map((n) => [n.id, n]));
    }
    const fmtValue = (value: SqlValue) =>
      typeof value === 'number' ? displaySize(value, attrs.unit) : '';
    const diff = attrs.data.diff;
    // Keyed so that the grid is re-created, with or without the diff columns,
    // when comparing starts or stops.
    const grid = m(DataGrid, {
      key: String(diff !== undefined),
      className: 'pf-tree-explorer__grid',
      fillHeight: true,
      // The tree explorer's own filter bar is the filtering surface;
      // row-level grid filters have no meaning on a tree walk.
      disableFilterControls: true,
      schema: {
        name: {
          title: 'Name',
          columnType: 'text',
          cellRenderer: (value: SqlValue, row: Row) => {
            const node = this.nodesById.get(Number(row.id));
            return renderNodeName(
              String(value),
              node,
              node !== undefined
                ? attrs.nodeIcon?.(node.propertyValues)
                : undefined,
              attrs.renderNodeTooltip,
            );
          },
        },
        total: {
          title: diff === undefined ? 'Total' : 'Current total',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        self: {
          title: diff === undefined ? 'Self' : 'Current self',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        percent:
          diff === undefined
            ? percentColumn('% of total')
            : percentColumn(
                'Current total (% of all)',
                'Current total / all visible current values',
              ),
        ...(diff !== undefined && {
          baselinePercent: percentColumn(
            'Baseline total (% of all)',
            'Baseline total / all visible baseline values',
          ),
          percentDelta: percentColumn(
            'Total Δ (% of all)',
            'Current total (% of all) − Baseline total (% of all), in percentage points',
            fmtPercentPointDelta,
          ),
          ...diffSchema(diff, attrs.unit, 'totalDelta'),
        }),
      },
      data: this.source,
      initialColumns:
        diff === undefined
          ? [
              {id: 'name', field: 'name'},
              {id: 'total', field: 'total', sort: 'DESC'},
              {id: 'self', field: 'self'},
              {id: 'percent', field: 'percent'},
            ]
          : [
              {id: 'name', field: 'name'},
              {id: 'total', field: 'total'},
              {id: 'baselineTotal', field: 'baselineTotal'},
              {id: 'totalDelta', field: 'totalDelta', sort: 'DESC'},
              {id: 'totalDeltaPercent', field: 'totalDeltaPercent'},
              {id: 'selfDelta', field: 'selfDelta'},
            ],
      tree: this.tree ?? DEFAULT_TREE,
      onTreeChanged: (tree) => {
        this.tree = tree;
      },
    });
    return [grid];
  }
}

// The columns of a diff. `pillColumn` gets flamegraph-colored pills matching
// the tab's legend; the other delta column gets green/red signed text.
function diffSchema(
  diff: TreeExplorerDiff,
  unit: string,
  pillColumn: 'totalDelta' | 'selfDelta',
) {
  const size = (title: string) => ({
    title,
    columnType: 'quantitative' as const,
    cellRenderer: (value: SqlValue) =>
      typeof value === 'number' ? displaySize(value, unit) : '',
  });
  const change = (title: string, pill: boolean) => ({
    title: renderInfoHeader(title, 'Current − Baseline'),
    titleString: title,
    columnType: 'quantitative' as const,
    cellRenderer: (value: SqlValue) =>
      pill ? renderChange(value, diff, unit) : renderSignedChange(value, unit),
  });
  const relativeDelta = (title: string, info: string) => ({
    title: renderInfoHeader(title, info),
    titleString: title,
    columnType: 'quantitative' as const,
    cellRenderer: fmtRelativeDelta,
    cellFormatter: formatRelativeDeltaText,
  });
  return {
    baselineTotal: size('Baseline total'),
    totalDelta: change('Total Δ', pillColumn === 'totalDelta'),
    totalDeltaPercent: relativeDelta(
      'Total Δ %',
      '(Current total − Baseline total) / Baseline total',
    ),
    baselineSelf: size('Baseline self'),
    selfDelta: change('Self Δ', pillColumn === 'selfDelta'),
    selfDeltaPercent: relativeDelta(
      'Self Δ %',
      '(Current self − Baseline self) / Baseline self',
    ),
  };
}

function renderSignedChange(value: SqlValue, unit: string): m.Children {
  if (typeof value !== 'number') {
    return '';
  }
  const sign: -1 | 0 | 1 = value > 0 ? 1 : value < 0 ? -1 : 0;
  return renderSignedText(displayChange(value, unit), sign);
}

function renderChange(
  value: SqlValue,
  diff: TreeExplorerDiff,
  unit: string,
): m.Children {
  if (typeof value !== 'number') {
    return '';
  }
  const text = displayChange(value, unit);
  const colors = diffColors(value, diff);
  if (colors === undefined) {
    return text;
  }
  return m(
    'span.pf-tree-explorer__change',
    {style: {backgroundColor: colors.background, color: colors.text}},
    text,
  );
}

// The diff columns of a row for a node's (or function's) values.
function diffCells(
  total: number,
  self: number,
  baselineTotal: number,
  baselineSelf: number,
): Row {
  return {
    baselineTotal,
    totalDelta: total - baselineTotal,
    totalDeltaPercent: relativeChange(baselineTotal, total) ?? Infinity,
    baselineSelf,
    selfDelta: self - baselineSelf,
    selfDeltaPercent: relativeChange(baselineSelf, self) ?? Infinity,
  };
}

// Tree-mode DataGrid source over resident tree explorer nodes. Children are
// indexed once; useRows() walks only the expanded rows, sorting siblings by
// the requested column (total descending by default).
class TreeExplorerTreeDataSource extends InMemoryDataSource {
  private readonly children = new Map<number, TreeExplorerNode[]>();
  private readonly roots: TreeExplorerNode[] = [];
  private readonly total: number;
  private readonly baselineTotal: number;
  private readonly isDiff: boolean;

  constructor(data: TreeExplorerData) {
    super([]);
    this.total = data.allRootsCumulativeValue;
    this.baselineTotal = data.diff?.allRootsBaselineCumulativeValue ?? 0;
    this.isDiff = data.diff !== undefined;
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
    // The grid reads cells by column alias, which is not the field for the
    // columns the user adds.
    const aliased = model.columns.filter((c) => c.alias !== c.field);
    const sortField = model.columns.find(
      (c) => c.alias === model.sort?.alias,
    )?.field;
    const cmp = rowComparator(
      sortField,
      model.sort?.direction,
      this.isDiff ? 'totalDelta' : 'total',
    );
    const rows: Row[] = [];
    const visit = (nodes: TreeExplorerNode[], depth: number) => {
      const siblings = nodes.map((n) => ({node: n, row: this.row(n, depth)}));
      siblings.sort((a, b) => cmp(a.row, b.row));
      for (const {node, row} of siblings) {
        for (const {field, alias} of aliased) {
          row[alias] = row[field] ?? null;
        }
        rows.push(row);
        const children = this.children.get(node.id);
        if (children !== undefined && isExpanded(node.id)) {
          visit(children, depth + 1);
        }
      }
    };
    visit(this.roots, 0);
    return rows;
  }

  private row(n: TreeExplorerNode, depth: number): Row {
    const percent = pctOfAll(n.cumulativeValue, this.total);
    const baselineCumulative = n.baselineCumulativeValue ?? 0;
    const baselinePercent = pctOfAll(baselineCumulative, this.baselineTotal);
    return {
      id: n.id,
      parentId: n.parentId,
      name: n.name,
      total: n.cumulativeValue,
      self: n.selfValue,
      percent,
      ...(this.isDiff && {
        ...diffCells(
          n.cumulativeValue,
          n.selfValue,
          baselineCumulative,
          n.baselineSelfValue ?? 0,
        ),
        baselinePercent,
        percentDelta: percent - baselinePercent,
      }),
      __id: n.id,
      __depth: depth,
      __has_children: this.children.has(n.id) ? 1 : 0,
    };
  }
}

// Sorts rows by the values in `field` (`defaultField` when the grid is not
// sorted), or alphabetically by name.
function rowComparator(
  field: string | undefined,
  direction: 'ASC' | 'DESC' | undefined,
  defaultField: string,
): (a: Row, b: Row) => number {
  const dir = direction === 'ASC' ? 1 : -1;
  if (field === 'name') {
    return (a, b) => dir * String(a.name).localeCompare(String(b.name));
  }
  const key = field ?? defaultField;
  return (a, b) => {
    const av = Number(a[key]);
    const bv = Number(b[key]);
    if (av === bv) return 0;
    return dir * (av - bv);
  };
}

export interface TreeExplorerFlatFunction {
  readonly name: string;
  readonly self: number;
  readonly total: number;
  // `self` and `total` in the baseline, when the tree is compared with one.
  readonly baselineSelf?: number;
  readonly baselineTotal?: number;
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

  const functions = new Map<
    string,
    {self: number; total: number; baselineSelf: number; baselineTotal: number}
  >();
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
        fn = {self: 0, total: 0, baselineSelf: 0, baselineTotal: 0};
        functions.set(name, fn);
      }
      fn.self += node.selfValue;
      fn.baselineSelf += node.baselineSelfValue ?? 0;
      const occurrences = onPath.get(name) ?? 0;
      if (occurrences === 0) {
        fn.total += node.cumulativeValue;
        fn.baselineTotal += node.baselineCumulativeValue ?? 0;
      }
      onPath.set(name, occurrences + 1);
      stack.push({node, exit: true});
      for (const child of children.get(node.id) ?? []) {
        stack.push({node: child, exit: false});
      }
    }
  }
  const compared = data.diff !== undefined;
  return [...functions].map(
    ([name, {self, total, baselineSelf, baselineTotal}]) => ({
      name,
      self,
      total,
      baselineSelf: compared ? baselineSelf : undefined,
      baselineTotal: compared ? baselineTotal : undefined,
    }),
  );
}

export interface TreeExplorerFlatViewAttrs {
  readonly data: TreeExplorerData;
  readonly unit: string;
  // See TreeExplorerPanelAttrs.
  readonly nodeIcon?: TreeExplorerNodeIconFn;
  readonly renderNodeTooltip?: TreeExplorerNodeTooltipRenderer;
}

// Flat per-function table (pprof "Top"): Name, Self, Self %, Total, Total %.
export class TreeExplorerFlatView implements m.ClassComponent<TreeExplorerFlatViewAttrs> {
  private rows?: readonly Row[];
  private flatDiff?: TreeExplorerDiff;
  private rowsData?: TreeExplorerData;
  private nodesByName = new Map<string, TreeExplorerNode[]>();

  view({attrs}: m.CVnode<TreeExplorerFlatViewAttrs>): m.Children {
    if (this.rows === undefined || this.rowsData !== attrs.data) {
      this.rowsData = attrs.data;
      const functions = computeFlatFunctions(attrs.data);
      this.rows = flatFunctionRows(attrs.data, functions);
      this.flatDiff =
        attrs.data.diff === undefined
          ? undefined
          : computeFlatDiff(attrs.data.diff, functions);
      this.nodesByName = nodesByName(attrs.data);
    }
    const fmtValue = (value: SqlValue) =>
      typeof value === 'number' ? displaySize(value, attrs.unit) : '';
    const diff = this.flatDiff;
    // Keyed as in TreeExplorerTreeView.
    const grid = m(DataGrid, {
      key: String(diff !== undefined),
      className: 'pf-tree-explorer__grid',
      fillHeight: true,
      disableFilterControls: true,
      schema: {
        name: {
          title: 'Name',
          columnType: 'text',
          cellRenderer: (value: SqlValue) => {
            const name = String(value);
            // A row merges every node with this name: show the icon of the
            // first node that has one.
            for (const node of this.nodesByName.get(name) ?? []) {
              const icon = attrs.nodeIcon?.(node.propertyValues);
              if (icon !== undefined) {
                return renderNodeName(
                  name,
                  node,
                  icon,
                  attrs.renderNodeTooltip,
                );
              }
            }
            return name;
          },
        },
        self: {
          title: diff === undefined ? 'Self' : 'Current self',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        selfPercent:
          diff === undefined
            ? percentColumn('Self %')
            : percentColumn(
                'Current self (% of all)',
                'Current self / all visible current values',
              ),
        ...(diff !== undefined && {
          baselineSelfPercent: percentColumn(
            'Baseline self (% of all)',
            'Baseline self / all visible baseline values',
          ),
          selfPercentDelta: percentColumn(
            'Self Δ (% of all)',
            'Current self (% of all) − Baseline self (% of all), in percentage points',
            fmtPercentPointDelta,
          ),
        }),
        total: {
          title: diff === undefined ? 'Total' : 'Current total',
          columnType: 'quantitative',
          cellRenderer: fmtValue,
        },
        totalPercent:
          diff === undefined
            ? percentColumn('Total %')
            : percentColumn(
                'Current total (% of all)',
                'Current total / all visible current values',
              ),
        ...(diff !== undefined && {
          baselineTotalPercent: percentColumn(
            'Baseline total (% of all)',
            'Baseline total / all visible baseline values',
          ),
          totalPercentDelta: percentColumn(
            'Total Δ (% of all)',
            'Current total (% of all) − Baseline total (% of all), in percentage points',
            fmtPercentPointDelta,
          ),
          ...diffSchema(diff, attrs.unit, 'selfDelta'),
        }),
      },
      data: this.rows,
      initialColumns:
        diff === undefined
          ? [
              {id: 'name', field: 'name'},
              {id: 'self', field: 'self', sort: 'DESC'},
              {id: 'selfPercent', field: 'selfPercent'},
              {id: 'total', field: 'total'},
              {id: 'totalPercent', field: 'totalPercent'},
            ]
          : [
              {id: 'name', field: 'name'},
              {id: 'self', field: 'self'},
              {id: 'baselineSelf', field: 'baselineSelf'},
              {id: 'selfDelta', field: 'selfDelta', sort: 'DESC'},
              {id: 'selfDeltaPercent', field: 'selfDeltaPercent'},
              {id: 'totalDelta', field: 'totalDelta'},
            ],
    });
    return [grid];
  }
}

// Name cell for the table views: `icon` (if any) followed by the name.
// Hovering the icon shows the output of `renderNodeTooltip`, called without
// default content since the tables have no node tooltip.
function renderNodeName(
  name: string,
  node: TreeExplorerNode | undefined,
  icon: string | undefined,
  renderNodeTooltip: TreeExplorerNodeTooltipRenderer | undefined,
): m.Children {
  if (node === undefined || icon === undefined) {
    return name;
  }
  const iconVnode = m(Icon, {className: 'pf-tree-explorer__node-icon', icon});
  const tooltip = renderNodeTooltip?.(
    {properties: node.propertyValues, node},
    null,
  );
  return m(
    'span.pf-tree-explorer__node-name',
    tooltip === undefined
      ? iconVnode
      : m(Tooltip, {trigger: iconVnode}, tooltip),
    name,
  );
}

// Groups nodes by name for the Functions view, which merges them per name.
function nodesByName(data: TreeExplorerData): Map<string, TreeExplorerNode[]> {
  const result = new Map<string, TreeExplorerNode[]>();
  for (const node of data.nodes) {
    let group = result.get(node.name);
    if (group === undefined) {
      group = [];
      result.set(node.name, group);
    }
    group.push(node);
  }
  return result;
}

function flatFunctionRows(
  data: TreeExplorerData,
  functions: ReadonlyArray<TreeExplorerFlatFunction>,
): readonly Row[] {
  const total = data.allRootsCumulativeValue;
  const baselineTotal = data.diff?.allRootsBaselineCumulativeValue ?? 0;
  return functions.map((fn) => {
    const selfPercent = pctOfAll(fn.self, total);
    const totalPercent = pctOfAll(fn.total, total);
    if (fn.baselineTotal === undefined || fn.baselineSelf === undefined) {
      return {
        name: fn.name,
        self: fn.self,
        selfPercent,
        total: fn.total,
        totalPercent,
      };
    }
    const baselineSelfPercent = pctOfAll(fn.baselineSelf, baselineTotal);
    const baselineTotalPercent = pctOfAll(fn.baselineTotal, baselineTotal);
    return {
      name: fn.name,
      self: fn.self,
      selfPercent,
      total: fn.total,
      totalPercent,
      ...diffCells(fn.total, fn.self, fn.baselineTotal, fn.baselineSelf),
      baselineSelfPercent,
      selfPercentDelta: selfPercent - baselineSelfPercent,
      baselineTotalPercent,
      totalPercentDelta: totalPercent - baselineTotalPercent,
    };
  });
}

function pctOfAll(value: number, root: number): number {
  return root === 0 ? 0 : (value / root) * 100;
}

function percentColumn(
  title: string,
  info?: string,
  cellRenderer: (value: SqlValue) => m.Children = fmtPercent,
) {
  return {
    title: info === undefined ? title : renderInfoHeader(title, info),
    titleString: title,
    columnType: 'quantitative' as const,
    cellRenderer,
  };
}

function renderInfoHeader(label: string, info: string): m.Children {
  return m(
    'span.pf-tree-explorer__info',
    label,
    m(
      Tooltip,
      {
        trigger: m(Icon, {
          className: 'pf-tree-explorer__info-icon',
          icon: 'help_outline',
        }),
      },
      info,
    ),
  );
}

function fmtPercent(value: SqlValue): string {
  return typeof value === 'number' ? `${value.toFixed(1)}%` : '';
}

function renderSignedText(text: string, sign: -1 | 0 | 1): m.Children {
  if (sign === 0) return text;
  return m(
    'span',
    {
      className:
        sign > 0
          ? 'pf-tree-explorer__delta--grew'
          : 'pf-tree-explorer__delta--shrank',
    },
    text,
  );
}

function fmtPercentPointDelta(value: SqlValue): m.Children {
  if (typeof value !== 'number') return '';
  return renderSignedText(
    displayPercentPointDelta(value, 1),
    roundedSign(value, 1),
  );
}

function formatRelativeDeltaText(value: SqlValue): string {
  if (typeof value !== 'number') return '';
  if (!Number.isFinite(value)) return 'new';
  const rounded = Number(value.toFixed(2));
  if (rounded === 0) return '0.00%';
  return `${rounded > 0 ? '+' : ''}${rounded.toFixed(2)}%`;
}

function fmtRelativeDelta(value: SqlValue): m.Children {
  if (typeof value !== 'number') return '';
  const sign = !Number.isFinite(value) ? 1 : roundedSign(value, 2);
  return renderSignedText(formatRelativeDeltaText(value), sign);
}

// Export of the flat function table. Values are raw numbers in the metric's
// unit so they can be aggregated in spreadsheets.
export function buildFlatExportString(
  data: TreeExplorerData,
  metric: TreeExplorerMetric,
  format: ExportFormat,
): string {
  const {diff} = data;
  const unitDisplay = getUnitDisplayName(metric.unit);
  const columns =
    diff === undefined
      ? ['name', 'self', 'selfPercent', 'total', 'totalPercent']
      : [
          'name',
          'self',
          'baselineSelf',
          'selfDelta',
          'selfDeltaPercent',
          'total',
          'baselineTotal',
          'totalDelta',
          'totalDeltaPercent',
        ];
  const metricUnit = `${metric.name} (${unitDisplay})`;
  const columnNames: Record<string, string> = {
    name: metric.nameColumnLabel ?? 'Name',
    self: `${diff ? 'Current self' : 'Self'} ${metricUnit}`,
    selfPercent: 'Self %',
    total: `${diff ? 'Current total' : 'Total'} ${metricUnit}`,
    totalPercent: 'Total %',
    baselineSelf: `Baseline self ${metricUnit}`,
    selfDelta: `Self delta ${metricUnit}`,
    selfDeltaPercent: 'Self delta %',
    baselineTotal: `Baseline total ${metricUnit}`,
    totalDelta: `Total delta ${metricUnit}`,
    totalDeltaPercent: 'Total delta %',
  };
  const total = data.allRootsCumulativeValue;
  const pct = (value: number) =>
    (total === 0 ? 0 : (value / total) * 100).toFixed(2);
  // Sorted like the view: by self, or by its change in a diff.
  const sortValue = (fn: TreeExplorerFlatFunction) =>
    fn.self - (fn.baselineSelf ?? 0);
  const rows = computeFlatFunctions(data)
    .sort((a, b) => sortValue(b) - sortValue(a))
    .map((fn) => ({
      name: fn.name,
      self: fn.self.toString(),
      selfPercent: pct(fn.self),
      total: fn.total.toString(),
      totalPercent: pct(fn.total),
      ...(fn.baselineTotal === undefined || fn.baselineSelf === undefined
        ? {}
        : {
            baselineSelf: fn.baselineSelf.toString(),
            selfDelta: (fn.self - fn.baselineSelf).toString(),
            selfDeltaPercent: exportRelativeChange(fn.baselineSelf, fn.self),
            baselineTotal: fn.baselineTotal.toString(),
            totalDelta: (fn.total - fn.baselineTotal).toString(),
            totalDeltaPercent: exportRelativeChange(fn.baselineTotal, fn.total),
          }),
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
