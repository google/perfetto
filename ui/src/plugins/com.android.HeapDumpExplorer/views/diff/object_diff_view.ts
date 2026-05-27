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

// Per-object diff. Loads `getInstance(currentEngine, currentId)` and
// `getInstance(baselineEngine, baselineId)` in parallel and renders both
// sides plus deltas for size, fields, and array elements. NEW / REMOVED
// renders a single side with the status banner.

import m from 'mithril';
import {Spinner} from '../../../../widgets/spinner';
import {EmptyState} from '../../../../widgets/empty_state';
import type {Engine} from '../../../../trace_processor/engine';
import {DataGrid} from '../../../../components/widgets/datagrid/datagrid';
import type {
  CellRenderResult,
  CellRenderer,
  SchemaRegistry,
} from '../../../../components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../../../trace_processor/query_result';
import type {InstanceDetail, InstanceRow, PrimOrRef} from '../../types';
import {
  pairObjects,
  type ObjectPairRow,
  type ObjectRowRaw,
} from '../../diff/object_pairing';
import {fmtHex} from '../../format';
import {type NavFn, Section, shortClassName} from '../../components';
import * as queries from '../../queries';
import {getActiveBaseline} from '../../baseline/state';
import type {HeapDump} from '../../queries';
import {
  deltaSizeRenderer,
  deltaCountRenderer,
  sideSizeRenderer,
  sideCountRenderer,
  statusRenderer,
} from '../../diff/diff_schemas';
import type {DiffStatus} from '../../diff/diff_rows';

interface ObjectDiffViewAttrs {
  readonly currentEngine: Engine;
  readonly baselineEngine: Engine;
  readonly activeDump: HeapDump;
  readonly currentId: number | null;
  readonly baselineId: number | null;
  readonly navigate: NavFn;
}

function renderPrimOrRef(v: PrimOrRef): string {
  return v.kind === 'prim' ? v.v : v.display;
}

function primOrRefEqual(a: PrimOrRef, b: PrimOrRef): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind === 'prim' && b.kind === 'prim') return a.v === b.v;
  if (a.kind === 'ref' && b.kind === 'ref') {
    // Pure ID equality is useless cross-trace as parser IDs are unstable.
    // As a heuristic for field-level diffs where full object pairing is
    // unavailable, we consider references equal if they point to objects
    // of the same class and with the same string value (if present).
    //
    // Note: `a.display` is formatted as "ClassName 0xAddress". We extract
    // the class name by taking the part before the space.
    const classA = a.display.split(' ')[0];
    const classB = b.display.split(' ')[0];
    return classA === classB && (a.str ?? '') === (b.str ?? '');
  }
  return false;
}

function fieldStatus(
  c: PrimOrRef | undefined,
  b: PrimOrRef | undefined,
): DiffStatus {
  if (c === undefined && b === undefined) return 'UNCHANGED';
  if (c === undefined) return 'REMOVED';
  if (b === undefined) return 'NEW';
  return primOrRefEqual(c, b) ? 'UNCHANGED' : 'GREW';
}

const FIELD_DIFF_SCHEMA: SchemaRegistry = {
  query: {
    name: {title: 'Name', columnType: 'text'},
    type_name: {title: 'Type', columnType: 'text'},
    status: {
      title: 'Status',
      columnType: 'text',
      cellRenderer: statusRenderer,
    },
    b_value: {title: 'Baseline', columnType: 'text'},
    c_value: {title: 'Current', columnType: 'text'},
  },
};

const FIELD_DIFF_COLS = [
  {id: 'name', field: 'name'},
  {id: 'type_name', field: 'type_name'},
  {id: 'status', field: 'status'},
  {id: 'b_value', field: 'b_value'},
  {id: 'c_value', field: 'c_value'},
];

const ARRAY_DIFF_SCHEMA: SchemaRegistry = {
  query: {
    idx: {title: 'Index', columnType: 'quantitative'},
    status: {
      title: 'Status',
      columnType: 'text',
      cellRenderer: statusRenderer,
    },
    b_value: {title: 'Baseline', columnType: 'text'},
    c_value: {title: 'Current', columnType: 'text'},
  },
};

const ARRAY_DIFF_COLS = [
  {id: 'idx', field: 'idx'},
  {id: 'status', field: 'status'},
  {id: 'b_value', field: 'b_value'},
  {id: 'c_value', field: 'c_value'},
];

interface SideValueRow extends Row {
  metric: string;
  baseline: number | null;
  current: number | null;
  delta: number | null;
}

// Per-row renderer dispatch: metrics whose name ends in 'Count' format
// values as plain integers, the rest as byte sizes. Folds the "size" and
// "count" rows into one grid so the section body holds a single child.
const SIZE_OR_COUNT_SCHEMA: SchemaRegistry = (() => {
  const isCountRow = (row: Row): boolean =>
    String(row.metric ?? '').endsWith('Count');
  const sideRenderer: CellRenderer = (v, row) =>
    isCountRow(row) ? sideCountRenderer(v, row) : sideSizeRenderer(v, row);
  const deltaRenderer: CellRenderer = (v, row) => {
    if (v === null) {
      return {
        content: m('span', {class: 'pf-hde-mono pf-hde-muted'}, '—'),
        align: 'right',
      } satisfies CellRenderResult;
    }
    return isCountRow(row)
      ? deltaCountRenderer(v, row)
      : deltaSizeRenderer(v, row);
  };
  return {
    query: {
      metric: {title: 'Metric', columnType: 'text'},
      baseline: {
        title: 'Baseline',
        columnType: 'quantitative',
        cellRenderer: sideRenderer,
      },
      current: {
        title: 'Current',
        columnType: 'quantitative',
        cellRenderer: sideRenderer,
      },
      delta: {
        title: 'Δ',
        columnType: 'quantitative',
        cellRenderer: deltaRenderer,
      },
    },
  };
})();

const SIZE_INITIAL_COLS = [
  {id: 'metric', field: 'metric'},
  {id: 'baseline', field: 'baseline'},
  {id: 'current', field: 'current'},
  {id: 'delta', field: 'delta'},
];

function diffStatus(
  c: InstanceDetail | null,
  b: InstanceDetail | null,
): DiffStatus {
  if (c && !b) return 'NEW';
  if (!c && b) return 'REMOVED';
  if (!c || !b) return 'UNCHANGED';
  const cR = c.row.retainedTotal;
  const bR = b.row.retainedTotal;
  if (cR > bR) return 'GREW';
  if (cR < bR) return 'SHRANK';
  return 'UNCHANGED';
}

function nullableDelta(c: number | null, b: number | null): number | null {
  if (c === null && b === null) return null;
  return (c ?? 0) - (b ?? 0);
}

function renderSizeTable(
  c: InstanceDetail | null,
  b: InstanceDetail | null,
): m.Children {
  function retainedTotal(d: InstanceDetail): {java: number; native_: number} {
    let java = 0;
    let nat = 0;
    for (const h of d.row.retainedByHeap) {
      java += h.java;
      nat += h.native_;
    }
    return {java, native_: nat};
  }
  const cRet = c ? retainedTotal(c) : null;
  const bRet = b ? retainedTotal(b) : null;
  const rows: SideValueRow[] = [
    {
      metric: 'Shallow',
      baseline: b?.row.shallowJava ?? null,
      current: c?.row.shallowJava ?? null,
      delta: nullableDelta(
        c?.row.shallowJava ?? null,
        b?.row.shallowJava ?? null,
      ),
    },
    {
      metric: 'Shallow Native',
      baseline: b?.row.shallowNative ?? null,
      current: c?.row.shallowNative ?? null,
      delta: nullableDelta(
        c?.row.shallowNative ?? null,
        b?.row.shallowNative ?? null,
      ),
    },
    {
      metric: 'Retained',
      baseline: bRet?.java ?? null,
      current: cRet?.java ?? null,
      delta: nullableDelta(cRet?.java ?? null, bRet?.java ?? null),
    },
    {
      metric: 'Retained Native',
      baseline: bRet?.native_ ?? null,
      current: cRet?.native_ ?? null,
      delta: nullableDelta(cRet?.native_ ?? null, bRet?.native_ ?? null),
    },
    {
      metric: 'Retained Count',
      baseline: b?.row.retainedCount ?? null,
      current: c?.row.retainedCount ?? null,
      delta: nullableDelta(
        c?.row.retainedCount ?? null,
        b?.row.retainedCount ?? null,
      ),
    },
  ];
  return m(DataGrid, {
    schema: SIZE_OR_COUNT_SCHEMA,
    rootSchema: 'query',
    data: rows,
    initialColumns: SIZE_INITIAL_COLS,
  });
}

function renderHeader(
  c: InstanceDetail | null,
  b: InstanceDetail | null,
  status: DiffStatus,
  navigate: NavFn,
): m.Children {
  const present = c ?? b;
  if (present === null) return null;
  const className = present.row.className;
  const heap = present.row.heap;
  const cId = c?.row.id ?? null;
  const bId = b?.row.id ?? null;
  // Plain header div (no card), matching non-diff ObjectView. The parent
  // pf-hde-view-stack already spaces it from the first Section — wrapping in
  // pf-hde-card + pf-hde-mb-4 stacks two margin sources and looks cramped.
  return m('div', [
    m('h2', {class: 'pf-hde-view-heading pf-hde-view-heading--tight'}, [
      `${shortClassName(className)} `,
      cId !== null ? fmtHex(cId) : bId !== null ? fmtHex(bId) : '',
      ' ',
      m(
        'span',
        {
          'class': statusClass(status),
          'aria-label': `Status: ${status}`,
        },
        statusLabel(status),
      ),
    ]),
    m('div', {class: 'pf-hde-info-grid'}, [
      m('span', {class: 'pf-hde-info-grid__label'}, 'Class:'),
      m('span', className),
      m('span', {class: 'pf-hde-info-grid__label'}, 'Heap:'),
      m('span', heap),
      cId !== null
        ? [
            m('span', {class: 'pf-hde-info-grid__label'}, 'Current id:'),
            m('span', [
              m(
                'button',
                {
                  class: 'pf-hde-link',
                  onclick: () =>
                    navigate('object', {id: cId, label: undefined}),
                },
                fmtHex(cId),
              ),
            ]),
          ]
        : null,
      bId !== null
        ? [
            m('span', {class: 'pf-hde-info-grid__label'}, 'Baseline id:'),
            m('span', {class: 'pf-hde-mono pf-hde-muted'}, fmtHex(bId)),
          ]
        : null,
    ]),
  ]);
}

function statusLabel(s: DiffStatus): string {
  switch (s) {
    case 'NEW':
      return 'NEW';
    case 'REMOVED':
      return 'REMOVED';
    case 'GREW':
      return 'GREW';
    case 'SHRANK':
      return 'SHRANK';
    case 'UNCHANGED':
      return 'unchanged';
  }
}

function statusClass(s: DiffStatus): string {
  return `pf-hde-status-text pf-hde-status-text--${s.toLowerCase()}`;
}

function renderFieldsDiff(
  c: InstanceDetail | null,
  b: InstanceDetail | null,
): m.Children {
  const cFields = c?.instanceFields ?? [];
  const bFields = b?.instanceFields ?? [];
  if (cFields.length === 0 && bFields.length === 0) return null;
  const byKey = new Map<
    string,
    {
      name: string;
      typeName: string;
      c?: PrimOrRef;
      b?: PrimOrRef;
    }
  >();
  for (const f of cFields) {
    const k = `${f.name}\x1f${f.typeName}`;
    byKey.set(k, {name: f.name, typeName: f.typeName, c: f.value});
  }
  for (const f of bFields) {
    const k = `${f.name}\x1f${f.typeName}`;
    const existing = byKey.get(k);
    if (existing) {
      existing.b = f.value;
    } else {
      byKey.set(k, {name: f.name, typeName: f.typeName, b: f.value});
    }
  }
  const rows: Row[] = [];
  for (const f of byKey.values()) {
    rows.push({
      name: f.name,
      type_name: f.typeName,
      status: fieldStatus(f.c, f.b),
      c_value: f.c ? renderPrimOrRef(f.c) : null,
      b_value: f.b ? renderPrimOrRef(f.b) : null,
    });
  }
  rows.sort((a, b) => String(a.name).localeCompare(String(b.name)));
  return m(
    Section,
    {title: 'Fields', defaultOpen: rows.length < 50},
    m(DataGrid, {
      schema: FIELD_DIFF_SCHEMA,
      rootSchema: 'query',
      data: rows,
      initialColumns: FIELD_DIFF_COLS,
    }),
  );
}

function instanceRowToRaw(r: InstanceRow): ObjectRowRaw {
  return {
    id: r.id,
    className: r.className,
    heapType: r.heap,
    valueString: r.str,
    arrayLength: null,
    shallow: r.shallowJava,
    shallowNative: r.shallowNative,
    retained: r.retainedTotal,
    retainedNative: 0,
    retainedCount: r.retainedCount,
  };
}

function buildInstanceDiffSchema(navigate: NavFn): SchemaRegistry {
  const idRenderer: CellRenderer = (value) => {
    if (value === null || value === undefined) {
      return {
        content: m('span', {class: 'pf-hde-mono pf-hde-muted'}, '—'),
        align: 'right',
      } satisfies CellRenderResult;
    }
    return {
      content: m('span', {class: 'pf-hde-mono'}, fmtHex(Number(value))),
      align: 'right',
    } satisfies CellRenderResult;
  };
  const objectRenderer: CellRenderer = (_value, row) => {
    const cls = String(row.cls ?? '');
    const cId = row.c_id == null ? null : Number(row.c_id);
    const bId = row.b_id == null ? null : Number(row.b_id);
    const str = row.str == null ? null : String(row.str);
    const displayId = cId ?? bId ?? 0;
    const display = `${shortClassName(cls)} ${fmtHex(displayId)}`;
    return {
      content: m(
        'button',
        {
          class: 'pf-hde-link',
          onclick: () =>
            navigate('object', {
              id: cId ?? bId ?? 0,
              currentId: cId,
              baselineId: bId,
              label: str ? `"${str.slice(0, 30)}"` : display,
            }),
        },
        display,
      ),
      align: 'left',
    } satisfies CellRenderResult;
  };
  return {
    query: {
      cls: {
        title: 'Object',
        columnType: 'text',
        cellRenderer: objectRenderer,
      },
      status: {
        title: 'Status',
        columnType: 'text',
        cellRenderer: statusRenderer,
      },
      delta_retained: {
        title: 'Δ Retained',
        columnType: 'quantitative',
        cellRenderer: deltaSizeRenderer,
      },
      b_retained: {
        title: 'Baseline Retained',
        columnType: 'quantitative',
        cellRenderer: sideSizeRenderer,
      },
      c_retained: {
        title: 'Current Retained',
        columnType: 'quantitative',
        cellRenderer: sideSizeRenderer,
      },
      str: {title: 'String Value', columnType: 'text'},
      c_id: {
        title: 'Current id',
        columnType: 'identifier',
        cellRenderer: idRenderer,
      },
      b_id: {
        title: 'Baseline id',
        columnType: 'identifier',
        cellRenderer: idRenderer,
      },
    },
  };
}

const INSTANCE_DIFF_COLS = [
  {id: 'cls', field: 'cls'},
  {id: 'status', field: 'status'},
  {id: 'delta_retained', field: 'delta_retained', sort: 'DESC' as const},
  {id: 'b_retained', field: 'b_retained'},
  {id: 'c_retained', field: 'c_retained'},
  {id: 'str', field: 'str'},
  // c_id / b_id are required by the Object column's click renderer —
  // InMemoryDataSource drops fields not declared as visible columns.
  {id: 'c_id', field: 'c_id'},
  {id: 'b_id', field: 'b_id'},
];

function pairToInstanceDiffRow(p: ObjectPairRow): Row {
  return {
    cls: p.className,
    status: p.status,
    str: p.valueString,
    c_id: p.c_id,
    b_id: p.b_id,
    delta_retained: p.delta_retained,
    c_retained: p.c_retained,
    b_retained: p.b_retained,
  };
}

function renderInstanceListDiff(
  title: string,
  current: ReadonlyArray<InstanceRow>,
  baseline: ReadonlyArray<InstanceRow>,
  navigate: NavFn,
): m.Children {
  if (current.length === 0 && baseline.length === 0) return null;
  const paired = pairObjects(
    current.map(instanceRowToRaw),
    baseline.map(instanceRowToRaw),
  );
  paired.sort(
    (a, b) => Math.abs(b.delta_retained) - Math.abs(a.delta_retained),
  );
  const rows = paired.map(pairToInstanceDiffRow);
  return m(
    Section,
    {
      title: `${title} (${paired.length})`,
      defaultOpen: paired.length > 0 && paired.length < 50,
    },
    m(DataGrid, {
      schema: buildInstanceDiffSchema(navigate),
      rootSchema: 'query',
      data: rows,
      initialColumns: INSTANCE_DIFF_COLS,
    }),
  );
}

function renderArrayDiff(
  c: InstanceDetail | null,
  b: InstanceDetail | null,
): m.Children {
  const cIsArr = c?.isArrayInstance ?? false;
  const bIsArr = b?.isArrayInstance ?? false;
  if (!cIsArr && !bIsArr) return null;
  const cElems = c?.arrayElems ?? [];
  const bElems = b?.arrayElems ?? [];
  const cByIdx = new Map<number, PrimOrRef>(
    cElems.map((e) => [e.idx, e.value]),
  );
  const bByIdx = new Map<number, PrimOrRef>(
    bElems.map((e) => [e.idx, e.value]),
  );
  const idxs = new Set<number>([...cByIdx.keys(), ...bByIdx.keys()]);
  const rows: Row[] = [];
  for (const i of idxs) {
    const cv = cByIdx.get(i);
    const bv = bByIdx.get(i);
    rows.push({
      idx: i,
      status: fieldStatus(cv, bv),
      c_value: cv ? renderPrimOrRef(cv) : null,
      b_value: bv ? renderPrimOrRef(bv) : null,
    });
  }
  rows.sort((a, b) => Number(a.idx) - Number(b.idx));
  const cLen = c?.arrayLength ?? 0;
  const bLen = b?.arrayLength ?? 0;
  const title = `Array Elements (current: ${cLen}, baseline: ${bLen})`;
  return m(
    Section,
    {title, defaultOpen: rows.length < 50},
    m(DataGrid, {
      schema: ARRAY_DIFF_SCHEMA,
      rootSchema: 'query',
      data: rows,
      initialColumns: ARRAY_DIFF_COLS,
    }),
  );
}

function ObjectDiffView(): m.Component<ObjectDiffViewAttrs> {
  let currentVnode: m.Vnode<ObjectDiffViewAttrs>;
  let current: InstanceDetail | null = null;
  let baseline: InstanceDetail | null = null;
  let loading = false;
  let error: string | null = null;
  let lastCid: number | null | undefined = undefined;
  let lastBid: number | null | undefined = undefined;
  let lastCe: Engine | null = null;
  let lastBe: Engine | null = null;

  async function load(
    currentEngine: Engine,
    baselineEngine: Engine,
    currentId: number | null,
    baselineId: number | null,
  ) {
    const primarySnap = currentVnode.attrs.activeDump;
    const baselineSnap = getActiveBaseline();
    if (baselineSnap === null) return;
    const isStale = () =>
      currentVnode.attrs.activeDump !== primarySnap ||
      getActiveBaseline() !== baselineSnap;
    loading = true;
    error = null;
    try {
      const [c, b] = await Promise.all([
        currentId !== null
          ? queries.getInstance(currentEngine, primarySnap, currentId)
          : Promise.resolve(null),
        baselineId !== null
          ? queries.getInstance(baselineEngine, baselineSnap.dump, baselineId)
          : Promise.resolve(null),
      ]);
      if (isStale()) return;
      current = c;
      baseline = b;
    } catch (err) {
      if (isStale()) return;
      error = err instanceof Error ? err.message : String(err);
      console.error('Object diff load failed:', err);
    } finally {
      loading = false;
      m.redraw();
    }
  }

  function ensureLoaded(
    currentEngine: Engine,
    baselineEngine: Engine,
    currentId: number | null,
    baselineId: number | null,
  ) {
    if (
      currentEngine === lastCe &&
      baselineEngine === lastBe &&
      currentId === lastCid &&
      baselineId === lastBid
    ) {
      return;
    }
    lastCe = currentEngine;
    lastBe = baselineEngine;
    lastCid = currentId;
    lastBid = baselineId;
    current = null;
    baseline = null;
    error = null;
    if (currentId !== null || baselineId !== null) {
      load(currentEngine, baselineEngine, currentId, baselineId).catch(
        console.error,
      );
    }
  }

  return {
    oninit(vnode) {
      currentVnode = vnode;
      ensureLoaded(
        vnode.attrs.currentEngine,
        vnode.attrs.baselineEngine,
        vnode.attrs.currentId,
        vnode.attrs.baselineId,
      );
    },
    onupdate(vnode) {
      currentVnode = vnode;
      ensureLoaded(
        vnode.attrs.currentEngine,
        vnode.attrs.baselineEngine,
        vnode.attrs.currentId,
        vnode.attrs.baselineId,
      );
    },
    view(vnode) {
      currentVnode = vnode;
      const {currentId, baselineId, navigate} = vnode.attrs;
      if (currentId === null && baselineId === null) {
        return m(EmptyState, {
          icon: 'memory',
          title: 'No object selected',
          fillHeight: true,
        });
      }
      if (loading && !current && !baseline) {
        return m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true}));
      }
      if (error) {
        return m(EmptyState, {
          icon: 'error',
          title: `Failed to load object diff: ${error}`,
          fillHeight: true,
        });
      }
      if (!current && !baseline) {
        return m(EmptyState, {
          icon: 'memory',
          title: 'Object not found',
          fillHeight: true,
        });
      }
      const status = diffStatus(current, baseline);
      return m('div', {class: 'pf-hde-view-scroll pf-hde-view-stack'}, [
        renderHeader(current, baseline, status, navigate),
        m(
          Section,
          {title: 'Object Size', defaultOpen: true},
          renderSizeTable(current, baseline),
        ),
        renderFieldsDiff(current, baseline),
        renderArrayDiff(current, baseline),
        renderInstanceListDiff(
          'Objects with References to this Object',
          current?.reverseRefs ?? [],
          baseline?.reverseRefs ?? [],
          navigate,
        ),
        renderInstanceListDiff(
          'Immediately Dominated Objects',
          current?.dominated ?? [],
          baseline?.dominated ?? [],
          navigate,
        ),
      ]);
    },
  };
}

export default ObjectDiffView;
