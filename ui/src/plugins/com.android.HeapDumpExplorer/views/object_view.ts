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
import type {Engine} from '../../../trace_processor/engine';
import {
  STR,
  type Row,
  type SqlValue,
} from '../../../trace_processor/query_result';
import {Spinner} from '../../../widgets/spinner';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import type {
  ColumnSchema,
  CellRenderResult,
} from '../../../components/widgets/datagrid/datagrid_schema';
import type {InstanceRow, InstanceDetail, PrimOrRef} from '../types';
import {download} from '../../../base/download_utils';
import {
  sizeRenderer,
  countRenderer,
  shortClassName,
  InstanceLink,
  Section,
  PrimOrRefCell,
  BitmapImage,
  COL_INFO,
  colHeader,
  fmtSize,
  fmtHex,
} from '../components';
import * as queries from '../queries';
import {Anchor} from '../../../widgets/anchor';
import {DetailsShell} from '../../../widgets/details_shell';
import {AsyncMemo} from '../../../base/async_memo';
import {type DumpRef, HdeAnchor} from '../nav';
import type {HeapDumpExplorerSession} from '../session';
import {
  METRIC_DOMINATED_OBJECT_SIZE,
  METRIC_OBJECT_SIZE,
} from './flamegraph_view';

interface ObjectViewAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly dump: queries.HeapDump;
  // Used to pivot the dump's flamegraph (see pivotFlamegraph).
  readonly id: number;
}

const JAVA_PRIM_SIZE: Record<string, number> = {
  boolean: 1,
  byte: 1,
  char: 2,
  short: 2,
  int: 4,
  float: 4,
  long: 8,
  double: 8,
};

function instanceRowToRow(r: InstanceRow): Row {
  let retained = 0;
  let retainedNative = 0;
  for (const h of r.retainedByHeap) {
    retained += h.java;
    retainedNative += h.native_;
  }
  return {
    id: r.id,
    cls: r.className,
    self_size: r.shallowJava,
    native_size: r.shallowNative,
    retained,
    retained_native: retainedNative,
    retained_count: r.retainedCount,
    reachable_size: r.reachableSize,
    reachable_native: r.reachableNative,
    reachable_count: r.reachableCount,
    heap: r.heap,
    str: r.str ?? null,
  };
}

type FieldRow = {name: string; typeName: string; value: PrimOrRef};

function fieldRowToRow(f: FieldRow): Row {
  const v = f.value;
  if (v.kind === 'ref') {
    return {
      name: f.name,
      type_name: f.typeName,
      value_display: v.display,
      value_kind: 'ref',
      ref_id: v.id,
      ref_str: v.str,
      shallow: v.shallowJava ?? 0,
      shallow_native: v.shallowNative ?? 0,
      retained: v.retainedJava ?? 0,
      retained_native: v.retainedNative ?? 0,
      reachable: v.reachableJava ?? null,
      reachable_native: v.reachableNative ?? null,
      reachable_count: v.reachableCount ?? null,
    };
  }
  return {
    name: f.name,
    type_name: f.typeName,
    value_display: v.v,
    value_kind: 'prim',
    ref_id: null,
    ref_str: null,
    shallow: JAVA_PRIM_SIZE[f.typeName] ?? 0,
    shallow_native: 0,
    retained: 0,
    retained_native: 0,
    reachable: null,
    reachable_native: null,
    reachable_count: null,
  };
}

type ArrayElemRow = {idx: number; value: PrimOrRef};

function arrayElemToRow(e: ArrayElemRow, elemTypeName: string): Row {
  const v = e.value;
  if (v.kind === 'ref') {
    return {
      idx: e.idx,
      value_display: v.display,
      value_kind: 'ref',
      ref_id: v.id,
      ref_str: v.str,
      shallow: v.shallowJava ?? 0,
      shallow_native: v.shallowNative ?? 0,
      retained: v.retainedJava ?? 0,
      retained_native: v.retainedNative ?? 0,
      reachable: v.reachableJava ?? null,
      reachable_native: v.reachableNative ?? null,
      reachable_count: v.reachableCount ?? null,
    };
  }
  return {
    idx: e.idx,
    value_display: v.v,
    value_kind: 'prim',
    ref_id: null,
    ref_str: null,
    shallow: JAVA_PRIM_SIZE[elemTypeName] ?? 0,
    shallow_native: 0,
    retained: 0,
    retained_native: 0,
    reachable: null,
    reachable_native: null,
    reachable_count: null,
  };
}

function nullableSizeRenderer(value: SqlValue): CellRenderResult {
  if (value === null) {
    return {
      content: m('span', {class: 'pf-hde-mono pf-hde-opacity-60'}, '\u2026'),
      align: 'right',
    };
  }
  return {
    content: m('span', {class: 'pf-hde-mono'}, fmtSize(Number(value ?? 0))),
    align: 'right',
  };
}

// Per-row info for the Object Size grid; the row label carries the metric.
const METRIC_INFO: Record<string, string> = {
  Shallow: 'Memory used by this object alone, excluding referenced objects.',
  Retained:
    'Memory exclusively held by this object (dominator subtree, self ' +
    'inclusive). The Native column is often zero in multi-rooted graphs ' +
    'where Bitmaps are reachable via multiple paths — see Reachable below.',
  Reachable:
    "Memory reachable from this object along the heap graph's BFS " +
    'shortest-path tree. Includes objects also reachable via other paths.',
};

const SIZE_SCHEMA: ColumnSchema = {
  metric: {
    title: 'Metric',
    columnType: 'text',
    cellRenderer: (value: SqlValue): CellRenderResult => {
      const label = String(value ?? '');
      const info = METRIC_INFO[label];
      return {content: info ? colHeader(label, info) : label};
    },
  },
  java: {
    title: 'Java',
    columnType: 'quantitative',
    cellRenderer: nullableSizeRenderer,
  },
  native: {
    title: 'Native',
    columnType: 'quantitative',
    cellRenderer: nullableSizeRenderer,
  },
  count: {
    title: 'Count',
    columnType: 'quantitative',
    cellRenderer: (value: SqlValue): CellRenderResult => {
      if (value === null) {
        return {
          content: m(
            'span',
            {class: 'pf-hde-mono pf-hde-opacity-60'},
            '\u2026',
          ),
          align: 'right',
        };
      }
      return {
        content: m(
          'span',
          {class: 'pf-hde-mono'},
          Number(value).toLocaleString(),
        ),
        align: 'right',
      };
    },
  },
};

function makeInstanceSchema(dump: DumpRef): ColumnSchema {
  return {
    id: {
      title: 'Object',
      columnType: 'identifier',
      cellRenderer: (value: SqlValue, row) => {
        const id = Number(value);
        const cls = String(row.cls ?? '');
        const display = `${shortClassName(cls)} ${fmtHex(id)}`;
        const str = row.str != null ? String(row.str) : null;
        return m('span', [
          m(HdeAnchor, {dump, to: {view: 'object', id}}, display),
          str
            ? m(
                'span',
                {class: 'pf-hde-str-badge'},
                ` "${str.length > 40 ? str.slice(0, 40) + '\u2026' : str}"`,
              )
            : null,
        ]);
      },
    },
    self_size: {
      title: colHeader('Shallow', COL_INFO.shallow),
      titleString: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    native_size: {
      title: colHeader('Shallow Native', COL_INFO.shallowNative),
      titleString: 'Shallow Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained: {
      title: colHeader('Retained', COL_INFO.retained),
      titleString: 'Retained',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained_native: {
      title: colHeader('Retained Native', COL_INFO.retainedNative),
      titleString: 'Retained Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained_count: {
      title: colHeader('Retained #', COL_INFO.retainedCount),
      titleString: 'Retained #',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    reachable_size: {
      title: colHeader('Reachable', COL_INFO.reachable),
      titleString: 'Reachable',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_native: {
      title: colHeader('Reachable Native', COL_INFO.reachableNative),
      titleString: 'Reachable Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_count: {
      title: colHeader('Reachable #', COL_INFO.reachableCount),
      titleString: 'Reachable #',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    heap: {
      title: 'Heap',
      columnType: 'text',
    },
    cls: {
      title: 'Class',
      columnType: 'text',
    },
    str: {
      title: 'String Value',
      columnType: 'text',
    },
  };
}

function makeFieldSchema(dump: DumpRef): ColumnSchema {
  return {
    name: {
      title: 'Name',
      columnType: 'text',
      cellRenderer: (value: SqlValue, row) => {
        if (row.value_kind === 'ref' && row.ref_id !== null) {
          return m(
            HdeAnchor,
            {dump, to: {view: 'object', id: Number(row.ref_id)}},
            String(value),
          );
        }
        return m('span', String(value ?? ''));
      },
    },
    type_name: {
      title: 'Type',
      columnType: 'text',
    },
    value_display: {
      title: 'Value',
      columnType: 'text',
      cellRenderer: (value: SqlValue, row) => {
        if (row.value_kind === 'ref' && row.ref_id !== null) {
          return m(PrimOrRefCell, {
            v: {
              kind: 'ref',
              id: Number(row.ref_id),
              display: String(value),
              str: row.ref_str != null ? String(row.ref_str) : null,
            },
            dump,
          });
        }
        return m('span', {class: 'pf-hde-mono'}, String(value ?? ''));
      },
    },
    shallow: {
      title: colHeader('Shallow', COL_INFO.shallow),
      titleString: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    shallow_native: {
      title: colHeader('Shallow Native', COL_INFO.shallowNative),
      titleString: 'Shallow Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained: {
      title: colHeader('Retained', COL_INFO.retained),
      titleString: 'Retained',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained_native: {
      title: colHeader('Retained Native', COL_INFO.retainedNative),
      titleString: 'Retained Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable: {
      title: colHeader('Reachable', COL_INFO.reachable),
      titleString: 'Reachable',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_native: {
      title: colHeader('Reachable Native', COL_INFO.reachableNative),
      titleString: 'Reachable Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_count: {
      title: colHeader('Reachable #', COL_INFO.reachableCount),
      titleString: 'Reachable #',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    value_kind: {
      title: 'Kind',
      columnType: 'text',
    },
    ref_id: {
      title: 'Ref ID',
      columnType: 'identifier',
    },
    ref_str: {
      title: 'Ref String',
      columnType: 'text',
    },
  };
}

function makeArraySchema(dump: DumpRef, elemTypeName: string): ColumnSchema {
  return {
    idx: {
      title: 'Index',
      columnType: 'quantitative',
      cellRenderer: (value: SqlValue): CellRenderResult => ({
        content: m('span', {class: 'pf-hde-mono'}, String(value ?? 0)),
        align: 'right',
      }),
    },
    value_display: {
      title: `Value (${elemTypeName})`,
      columnType: 'text',
      cellRenderer: (value: SqlValue, row) => {
        if (row.value_kind === 'ref' && row.ref_id !== null) {
          return m(PrimOrRefCell, {
            v: {
              kind: 'ref',
              id: Number(row.ref_id),
              display: String(value),
              str: row.ref_str != null ? String(row.ref_str) : null,
            },
            dump,
          });
        }
        return m('span', {class: 'pf-hde-mono'}, String(value ?? ''));
      },
    },
    shallow: {
      title: colHeader('Shallow', COL_INFO.shallow),
      titleString: 'Shallow',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    shallow_native: {
      title: colHeader('Shallow Native', COL_INFO.shallowNative),
      titleString: 'Shallow Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained: {
      title: colHeader('Retained', COL_INFO.retained),
      titleString: 'Retained',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    retained_native: {
      title: colHeader('Retained Native', COL_INFO.retainedNative),
      titleString: 'Retained Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable: {
      title: colHeader('Reachable', COL_INFO.reachable),
      titleString: 'Reachable',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_native: {
      title: colHeader('Reachable Native', COL_INFO.reachableNative),
      titleString: 'Reachable Native',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    reachable_count: {
      title: colHeader('Reachable #', COL_INFO.reachableCount),
      titleString: 'Reachable #',
      columnType: 'quantitative',
      cellRenderer: countRenderer,
    },
    value_kind: {
      title: 'Kind',
      columnType: 'text',
    },
    ref_id: {
      title: 'Ref ID',
      columnType: 'identifier',
    },
    ref_str: {
      title: 'Ref String',
      columnType: 'text',
    },
  };
}

export function ObjectView(): m.Component<ObjectViewAttrs> {
  const dataMemo = new AsyncMemo<InstanceDetail | undefined>();

  async function enrichDetail(engine: Engine, d: InstanceDetail) {
    // Enrich all sections with reachable sizes asynchronously.
    const enrichTasks: Promise<void>[] = [
      queries.enrichWithReachable(engine, [d.row]),
      queries.enrichWithReachable(engine, d.reverseRefs),
      queries.enrichWithReachable(engine, d.dominated),
    ];
    if (d.isClassObj) {
      enrichTasks.push(
        queries.enrichFieldsWithReachable(engine, d.staticFields),
      );
    }
    if (d.isClassInstance && d.instanceFields.length > 0) {
      enrichTasks.push(
        queries.enrichFieldsWithReachable(engine, d.instanceFields),
      );
    }
    if (d.isArrayInstance) {
      enrichTasks.push(
        queries.enrichArrayElemsWithReachable(engine, d.arrayElems),
      );
    }

    await Promise.all(enrichTasks);
  }

  return {
    onremove() {
      dataMemo.dispose();
    },
    view({attrs}) {
      const {dump, id, session} = attrs;
      const {isPending, data: detail} = dataMemo.use({
        key: id,
        compute: async () => {
          const detail = await queries.getInstance(
            session.trace.engine,
            dump,
            id,
          );
          // TODO: Show intermediate state using multiple asynmemos
          if (detail) await enrichDetail(session.trace.engine, detail);
          return detail;
        },
      });

      if (isPending) {
        return m(
          DetailsShell,
          {
            title: `Object ${fmtHex(id)}`,
            fillHeight: true,
            className: 'pf-hde-tab--padded',
          },
          m('div', {class: 'pf-hde-loading'}, m(Spinner, {easing: true})),
        );
      }

      if (!detail) {
        return m(
          DetailsShell,
          {
            title: `Object ${fmtHex(id)}`,
            fillHeight: true,
            className: 'pf-hde-tab--padded',
          },
          m(
            'div',
            {class: 'pf-hde-error-text'},
            'No object with id ' + fmtHex(id),
          ),
        );
      }

      const {row} = detail;

      const flamegraphAction = (isDominator: boolean) =>
        row.className
          ? m(
              HdeAnchor,
              {
                dump,
                to: {view: 'flamegraph'},
                title: isDominator
                  ? 'Open in Flamegraph pivoted on this dominator path'
                  : 'Open in Flamegraph pivoted on this shortest path',
                onclick: () => {
                  void pivotFlamegraph(
                    session,
                    dump,
                    row.id,
                    row.className,
                    isDominator,
                  );
                },
              },
              'View in Flamegraph',
            )
          : null;

      return m(
        DetailsShell,
        {
          title: 'Object ' + fmtHex(row.id),
          fillHeight: true,
          className: 'pf-hde-tab--padded',
        },
        m('div', {class: 'pf-hde-view-scroll pf-hde-view-stack'}, [
          m('div', {class: 'pf-hde-action-row'}, [
            m(InstanceLink, {row, dump}),
          ]),

          detail.bitmap
            ? m(Section, {title: 'Bitmap Image'}, [
                m(BitmapImage, {
                  width: detail.bitmap.width,
                  height: detail.bitmap.height,
                  format: detail.bitmap.format,
                  data: detail.bitmap.data,
                }),
                m('div', {class: 'pf-hde-bitmap-meta pf-hde-mt-1'}, [
                  m(
                    'span',
                    detail.bitmap.width +
                      ' x ' +
                      detail.bitmap.height +
                      ' px (' +
                      detail.bitmap.format.toUpperCase() +
                      ')',
                  ),
                  m(
                    Anchor,
                    {
                      class: 'pf-hde-download-link',
                      onclick: () => {
                        if (detail.bitmap === null) return;
                        const ext = detail.bitmap.format;
                        void download({
                          content: detail.bitmap.data,
                          fileName: `bitmap-${fmtHex(row.id)}.${ext}`,
                        });
                      },
                    },
                    'Download image',
                  ),
                ]),
              ])
            : null,

          m(
            Section,
            {
              title: 'Shortest Path from GC Root',
              actions: detail.shortestPath ? flamegraphAction(false) : null,
            },
            detail.shortestPath
              ? m(
                  'div',
                  {class: 'pf-hde-view-stack--tight'},
                  detail.shortestPath.map((pe, i) =>
                    m(
                      'div',
                      {
                        key: i,
                        class: 'pf-hde-path-entry',
                        style: {'--pf-hde-depth': String(i)},
                      },
                      [
                        m(
                          'span',
                          {class: 'pf-hde-path-arrow'},
                          i === 0 ? '' : '\u2192',
                        ),
                        m(InstanceLink, {row: pe.row, dump}),
                        pe.field
                          ? m('span', {class: 'pf-hde-path-field'}, pe.field)
                          : null,
                      ],
                    ),
                  ),
                )
              : m('p', {class: 'pf-hde-muted'}, 'No path to GC root.'),
          ),

          m(
            Section,
            {
              title: 'Dominator Tree Path',
              actions: detail.dominatorPath ? flamegraphAction(true) : null,
            },
            detail.dominatorPath
              ? m(
                  'div',
                  {class: 'pf-hde-view-stack--tight'},
                  detail.dominatorPath.map((pe, i) =>
                    m(
                      'div',
                      {
                        key: i,
                        class: `pf-hde-path-entry${pe.isDominator ? ' pf-hde-semibold' : ''}`,
                        style: {'--pf-hde-depth': String(i)},
                      },
                      [
                        m(
                          'span',
                          {class: 'pf-hde-path-arrow'},
                          i === 0 ? '' : '\u2192',
                        ),
                        m(InstanceLink, {row: pe.row, dump}),
                        pe.field
                          ? m('span', {class: 'pf-hde-path-field'}, pe.field)
                          : null,
                      ],
                    ),
                  ),
                )
              : m('p', {class: 'pf-hde-muted'}, 'No path to GC root.'),
          ),

          m(Section, {title: 'Object Info'}, [
            m('div', {class: 'pf-hde-info-grid'}, [
              m('span', {class: 'pf-hde-info-grid__label'}, 'Class:'),
              m(
                'span',
                detail.classObjRow
                  ? m(InstanceLink, {
                      row: detail.classObjRow,
                      dump,
                    })
                  : '???',
              ),
              m('span', {class: 'pf-hde-info-grid__label'}, 'Heap:'),
              m('span', row.heap),
              ...(row.isRoot
                ? [
                    m(
                      'span',
                      {class: 'pf-hde-info-grid__label'},
                      'Root Types:',
                    ),
                    m('span', row.rootTypeNames?.join(', ')),
                  ]
                : []),
            ]),
          ]),

          m(
            Section,
            {title: 'Object Size'},
            (() => {
              let retainedJava = 0;
              let retainedNative = 0;
              for (const h of row.retainedByHeap) {
                retainedJava += h.java;
                retainedNative += h.native_;
              }
              const sizeRows: Row[] = [
                {
                  metric: 'Shallow',
                  java: row.shallowJava,
                  native: row.shallowNative,
                  count: 1,
                },
                {
                  metric: 'Retained',
                  java: retainedJava,
                  native: retainedNative,
                  count: row.retainedCount,
                },
                {
                  metric: 'Reachable',
                  java: row.reachableSize,
                  native: row.reachableNative,
                  count: row.reachableCount,
                },
              ];
              return m(DataGrid, {
                schema: SIZE_SCHEMA,
                data: sizeRows,
                initialColumns: [
                  {id: 'metric', field: 'metric'},
                  {id: 'java', field: 'java'},
                  {id: 'native', field: 'native'},
                  {id: 'count', field: 'count'},
                ],
              });
            })(),
          ),

          detail.isClassObj
            ? m(Section, {title: 'Class Info'}, [
                m('div', {class: 'pf-hde-info-grid pf-hde-mb-3'}, [
                  m(
                    'span',
                    {class: 'pf-hde-info-grid__label'},
                    'Instance Size:',
                  ),
                  m(
                    'span',
                    {class: 'pf-hde-mono'},
                    String(detail.instanceSize),
                  ),
                ]),
              ])
            : null,

          detail.classHierarchy.length > 0
            ? m(
                Section,
                {title: 'Class Hierarchy'},
                renderClassHierarchy(dump, detail.classHierarchy),
              )
            : null,

          detail.isClassObj
            ? m(
                Section,
                {title: 'Static Fields'},
                renderFieldsGrid(detail.staticFields, dump),
              )
            : null,

          detail.isClassInstance
            ? m(
                Section,
                {title: 'Fields'},
                detail.instanceFields.length > 0
                  ? renderFieldsGrid(detail.instanceFields, dump)
                  : m('p', {class: 'pf-hde-muted'}, 'No instance fields.'),
              )
            : null,

          detail.isArrayInstance
            ? m(
                Section,
                {title: `Array Elements (${detail.arrayLength})`},
                renderArrayGrid(
                  detail.arrayElems,
                  detail.elemTypeName ?? 'Object',
                  dump,
                  detail.elemTypeName === 'byte'
                    ? () => {
                        queries
                          .getRawArrayBlob(session.trace.engine, id)
                          .then((blob) => {
                            if (blob !== null) {
                              void download({
                                content: blob,
                                fileName: `array-${fmtHex(id)}.bin`,
                              });
                            }
                          })
                          .catch(console.error);
                      }
                    : undefined,
                ),
              )
            : null,

          m(
            Section,
            {
              title:
                detail.reverseRefs.length > 0
                  ? `Objects with References to this Object (${detail.reverseRefs.length})`
                  : 'Objects with References to this Object',
              defaultOpen:
                detail.reverseRefs.length > 0 && detail.reverseRefs.length < 50,
            },
            detail.reverseRefs.length > 0
              ? m(DataGrid, {
                  schema: makeInstanceSchema(dump),
                  data: detail.reverseRefs.map(instanceRowToRow),
                  initialColumns: [
                    {id: 'id', field: 'id'},
                    {id: 'cls', field: 'cls'},
                    {id: 'str', field: 'str'},
                    {id: 'self_size', field: 'self_size'},
                    {id: 'native_size', field: 'native_size'},
                    {id: 'retained', field: 'retained'},
                    {id: 'retained_native', field: 'retained_native'},
                    {id: 'retained_count', field: 'retained_count'},
                    {id: 'reachable_size', field: 'reachable_size'},
                    {id: 'reachable_native', field: 'reachable_native'},
                    {id: 'reachable_count', field: 'reachable_count'},
                  ],
                  showExportButton: true,
                })
              : m(
                  'p',
                  {class: 'pf-hde-muted'},
                  'No references to this object.',
                ),
          ),

          m(
            Section,
            {
              title:
                detail.dominated.length > 0
                  ? `Immediately Dominated Objects (${detail.dominated.length})`
                  : 'Immediately Dominated Objects',
              defaultOpen:
                detail.dominated.length > 0 && detail.dominated.length < 50,
            },
            detail.dominated.length > 0
              ? m(DataGrid, {
                  schema: makeInstanceSchema(dump),
                  data: detail.dominated.map(instanceRowToRow),
                  initialColumns: [
                    {id: 'id', field: 'id'},
                    {id: 'cls', field: 'cls'},
                    {id: 'str', field: 'str'},
                    {id: 'self_size', field: 'self_size'},
                    {id: 'native_size', field: 'native_size'},
                    {id: 'retained', field: 'retained'},
                    {id: 'retained_native', field: 'retained_native'},
                    {id: 'retained_count', field: 'retained_count'},
                    {id: 'reachable_size', field: 'reachable_size'},
                    {id: 'reachable_native', field: 'reachable_native'},
                    {id: 'reachable_count', field: 'reachable_count'},
                    {id: 'heap', field: 'heap'},
                  ],
                  showExportButton: true,
                })
              : m(
                  'p',
                  {class: 'pf-hde-muted'},
                  'No immediately dominated objects.',
                ),
          ),
        ]),
      );
    },
  };
}

function renderFieldsGrid(fields: FieldRow[], dump: DumpRef): m.Children {
  if (fields.length === 0) {
    return m('div', {class: 'pf-hde-info-grid__label'}, 'No fields');
  }
  return m(DataGrid, {
    schema: makeFieldSchema(dump),
    data: fields.map(fieldRowToRow),
    initialColumns: [
      {id: 'type_name', field: 'type_name'},
      {id: 'name', field: 'name'},
      {id: 'value_display', field: 'value_display'},
      {id: 'shallow', field: 'shallow'},
      {id: 'shallow_native', field: 'shallow_native'},
      {id: 'retained', field: 'retained'},
      {id: 'retained_native', field: 'retained_native'},
      {id: 'reachable', field: 'reachable'},
      {id: 'reachable_native', field: 'reachable_native'},
      {id: 'reachable_count', field: 'reachable_count'},
      {id: 'value_kind', field: 'value_kind'},
      {id: 'ref_id', field: 'ref_id'},
      {id: 'ref_str', field: 'ref_str'},
    ],
    showExportButton: true,
  });
}

function renderArrayGrid(
  elems: ArrayElemRow[],
  elemTypeName: string,
  dump: DumpRef,
  onDownloadBytes?: () => void,
): m.Children {
  function copyTsv() {
    const header = 'Index\tValue';
    const lines = elems.map(
      (e) =>
        e.idx + '\t' + (e.value.kind === 'prim' ? e.value.v : e.value.display),
    );
    navigator.clipboard
      .writeText(header + '\n' + lines.join('\n'))
      .catch(console.error);
  }

  return m('div', [
    onDownloadBytes || elems.length > 0
      ? m('div', {class: 'pf-hde-action-row pf-hde-mb-2'}, [
          onDownloadBytes
            ? m(
                Anchor,
                {class: 'pf-hde-download-link', onclick: onDownloadBytes},
                'Download bytes',
              )
            : null,
          elems.length > 0
            ? m(
                Anchor,
                {class: 'pf-hde-download-link', onclick: copyTsv},
                'Copy as TSV',
              )
            : null,
        ])
      : null,
    m(DataGrid, {
      schema: makeArraySchema(dump, elemTypeName),
      data: elems.map((e) => arrayElemToRow(e, elemTypeName)),
      initialColumns: [
        {id: 'idx', field: 'idx'},
        {id: 'value_display', field: 'value_display'},
        {id: 'shallow', field: 'shallow'},
        {id: 'shallow_native', field: 'shallow_native'},
        {id: 'retained', field: 'retained'},
        {id: 'retained_native', field: 'retained_native'},
        {id: 'reachable', field: 'reachable'},
        {id: 'reachable_native', field: 'reachable_native'},
        {id: 'reachable_count', field: 'reachable_count'},
        {id: 'value_kind', field: 'value_kind'},
        {id: 'ref_id', field: 'ref_id'},
        {id: 'ref_str', field: 'ref_str'},
      ],
      showExportButton: true,
    }),
  ]);
}

// Look up the object's path_hash in the chosen tree (BFS or dominator) and
// pivot the dump's flamegraph on it with the matching metric. The hash is
// tree-specific so the tree dictates both. The chip shows
// `<class> (this instance)` since the raw hash regex is unreadable. No-op if
// the object has no entry (e.g. unreachable garbage). Navigation to the
// flamegraph is left to the link this runs from.
async function pivotFlamegraph(
  session: HeapDumpExplorerSession,
  dump: DumpRef,
  id: number,
  cls: string,
  isDominator: boolean,
): Promise<void> {
  const engine = session.trace.engine;
  const moduleName = isDominator
    ? 'android.memory.heap_graph.dominator_class_tree'
    : 'android.memory.heap_graph.class_tree';
  const table = isDominator
    ? '_heap_graph_dominator_path_hashes'
    : '_heap_graph_path_hashes';
  await engine.query(`INCLUDE PERFETTO MODULE ${moduleName};`);
  const res = await engine.query(
    `SELECT CAST(path_hash AS TEXT) AS path_hash
       FROM ${table} WHERE id = ${id} LIMIT 1`,
  );
  const it = res.iter({path_hash: STR});
  if (!it.valid()) return;
  session.setFlamegraphPanelState(dump, {
    selectedMetricId: isDominator
      ? METRIC_DOMINATED_OBJECT_SIZE
      : METRIC_OBJECT_SIZE,
    addedMetricIds: [],
    displayMode: 'flamegraph',
    filters: [],
    view: {
      kind: 'PIVOT',
      pivot: `/^${it.path_hash}$/`,
      displayLabel: `${shortClassName(cls)} (this instance)`,
    },
  });
}

// `java.lang.Class<Foo>` has no useful subclasses in heap_graph_class; the
// meaningful filter target is `Foo`.
const CLASS_OBJ_PREFIX = 'java.lang.Class<';
function subclassFilterTarget(className: string): string {
  if (className.startsWith(CLASS_OBJ_PREFIX) && className.endsWith('>')) {
    return className.slice(CLASS_OBJ_PREFIX.length, -1);
  }
  return className;
}

function classFilterLink(dump: queries.HeapDump, className: string): m.Child {
  return m(
    HdeAnchor,
    {
      title: 'Open subclasses of this class',
      dump,
      to: {view: 'classes', rootClass: subclassFilterTarget(className)},
    },
    className,
  );
}

function renderClassHierarchy(
  dump: queries.HeapDump,
  hierarchy: string[],
): m.Children {
  const topDown = hierarchy.slice().reverse();
  return m(
    'div',
    {class: 'pf-hde-view-stack--tight'},
    topDown.map((className, i) =>
      m(
        'div',
        {
          key: className,
          class: `pf-hde-path-entry${i === topDown.length - 1 ? ' pf-hde-semibold' : ''}`,
          style: {'--pf-hde-depth': String(i)},
        },
        [
          m('span', {class: 'pf-hde-path-arrow'}, i === 0 ? '' : '→'),
          classFilterLink(dump, className),
        ],
      ),
    ),
  );
}
