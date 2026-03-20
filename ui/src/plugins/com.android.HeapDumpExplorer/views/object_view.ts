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
import type {SqlValue} from '../../../trace_processor/query_result';
import type {Row} from '../../../trace_processor/query_result';
import {Spinner} from '../../../widgets/spinner';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import type {
  SchemaRegistry,
  CellRenderResult,
} from '../../../components/widgets/datagrid/datagrid_schema';
import type {InstanceRow, InstanceDetail, HeapInfo, PrimOrRef} from '../types';
import {fmtSize, fmtHex} from '../format';
import {downloadBlob} from '../download';
import {
  type NavFn,
  sizeRenderer,
  shortClassName,
  InstanceLink,
  Section,
  PrimOrRefCell,
  BitmapImage,
} from '../components';
import * as queries from '../queries';

export interface ObjectParams {
  id: number;
}

interface ObjectViewAttrs {
  engine: Engine;
  heaps: HeapInfo[];
  navigate: NavFn;
  params: ObjectParams;
  onViewInTimeline?: (objectId: number) => void;
}

// ─── Size helpers ─────────────────────────────────────────────────────────────

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

// ─── Row conversion helpers ──────────────────────────────────────────────────

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
  };
}

// ─── Schema factories ────────────────────────────────────────────────────────

function nullableSizeRenderer(value: SqlValue): CellRenderResult {
  if (value === null) {
    return {
      content: m('span', {class: 'ah-mono ah-opacity-60'}, '\u2026'),
      align: 'right',
    };
  }
  return {
    content: m('span', {class: 'ah-mono'}, fmtSize(Number(value ?? 0))),
    align: 'right',
  };
}

const SIZE_SCHEMA: SchemaRegistry = {
  query: {
    metric: {
      title: 'Metric',
      columnType: 'text',
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
  },
};

function makeInstanceSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      id: {
        title: 'Object',
        columnType: 'identifier',
        cellRenderer: (value: SqlValue, row) => {
          const id = Number(value);
          const cls = String(row.cls ?? '');
          const display = `${shortClassName(cls)} ${fmtHex(id)}`;
          const str = row.str != null ? String(row.str) : null;
          return m('span', [
            m(
              'button',
              {
                class: 'ah-link',
                onclick: () =>
                  navigate('object', {id, label: str ? `"${str}"` : display}),
              },
              display,
            ),
            str
              ? m(
                  'span',
                  {class: 'ah-str-badge'},
                  ` "${str.length > 40 ? str.slice(0, 40) + '\u2026' : str}"`,
                )
              : null,
          ]);
        },
      },
      self_size: {
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      native_size: {
        title: 'Shallow Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained: {
        title: 'Retained',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained_native: {
        title: 'Retained Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
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
    },
  };
}

function makeFieldSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      name: {
        title: 'Name',
        columnType: 'text',
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
              navigate,
            });
          }
          return m('span', {class: 'ah-mono'}, String(value ?? ''));
        },
      },
      shallow: {
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      shallow_native: {
        title: 'Shallow Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained: {
        title: 'Retained',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained_native: {
        title: 'Retained Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
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
    },
  };
}

function makeArraySchema(
  navigate: NavFn,
  elemTypeName: string,
): SchemaRegistry {
  return {
    query: {
      idx: {
        title: 'Index',
        columnType: 'quantitative',
        cellRenderer: (value: SqlValue): CellRenderResult => ({
          content: m('span', {class: 'ah-mono'}, String(value ?? 0)),
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
              navigate,
            });
          }
          return m('span', {class: 'ah-mono'}, String(value ?? ''));
        },
      },
      shallow: {
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      shallow_native: {
        title: 'Shallow Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained: {
        title: 'Retained',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      retained_native: {
        title: 'Retained Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
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
    },
  };
}

// ─── ObjectView ──────────────────────────────────────────────────────────────

function ObjectView(): m.Component<ObjectViewAttrs> {
  let detail: InstanceDetail | null | 'loading' = 'loading';
  let prevId: number | undefined;
  let alive = true;
  let fetchSeq = 0;

  function fetchData(attrs: ObjectViewAttrs) {
    detail = 'loading';
    prevId = attrs.params.id;
    const seq = ++fetchSeq;
    queries
      .getInstance(attrs.engine, attrs.params.id)
      .then((d) => {
        if (!alive || seq !== fetchSeq) return;
        detail = d;
        m.redraw();
        if (d) {
          queries.enrichWithReachable(attrs.engine, [d.row]).then(() => {
            if (alive && seq === fetchSeq) m.redraw();
          });
        }
      })
      .catch((err) => {
        console.error(err);
        if (!alive || seq !== fetchSeq) return;
        detail = null;
        m.redraw();
      });
  }

  return {
    oninit(vnode) {
      fetchData(vnode.attrs);
    },
    onupdate(vnode) {
      if (vnode.attrs.params.id !== prevId) {
        fetchData(vnode.attrs);
      }
    },
    onremove() {
      alive = false;
    },
    view(vnode) {
      const {navigate, params, onViewInTimeline} = vnode.attrs;

      if (detail === 'loading') {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }
      if (!detail) {
        return m(
          'div',
          {class: 'ah-error-text'},
          'No object with id ' + fmtHex(params.id),
        );
      }

      const {row} = detail;

      return m('div', {class: 'ah-view-scroll ah-view-stack'}, [
        m('div', [
          m(
            'h2',
            {
              class: 'ah-view-heading',
              style: {marginBottom: '0.25rem'},
            },
            'Object ' + fmtHex(row.id),
          ),
          m(
            'div',
            {style: {display: 'flex', alignItems: 'center', gap: '0.75rem'}},
            [
              m(InstanceLink, {row, navigate}),
              onViewInTimeline
                ? m(
                    'button',
                    {
                      class: 'ah-download-link',
                      onclick: () => onViewInTimeline(row.id),
                    },
                    'View in Timeline',
                  )
                : null,
            ],
          ),
        ]),

        detail.bitmap
          ? m(Section, {title: 'Bitmap Image'}, [
              m(BitmapImage, {
                width: detail.bitmap.width,
                height: detail.bitmap.height,
                format: detail.bitmap.format,
                data: detail.bitmap.data,
              }),
              m(
                'div',
                {
                  class: 'ah-mt-1',
                  style: {
                    display: 'flex',
                    alignItems: 'center',
                    gap: '0.75rem',
                    fontSize: '0.75rem',
                    lineHeight: '1rem',
                    color: 'var(--ah-text-muted)',
                  },
                },
                [
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
                    'button',
                    {
                      class: 'ah-download-link',
                      onclick: () => {
                        if (
                          detail === null ||
                          detail === 'loading' ||
                          detail.bitmap === null
                        ) {
                          return;
                        }
                        const ext = detail.bitmap.format;
                        downloadBlob(
                          `bitmap-${fmtHex(row.id)}.${ext}`,
                          detail.bitmap.data,
                        );
                      },
                    },
                    'Download image',
                  ),
                ],
              ),
            ])
          : null,

        detail.pathFromRoot
          ? m(
              Section,
              {
                title: detail.isUnreachablePath
                  ? 'Sample Path'
                  : 'Sample Path from GC Root',
              },
              m(
                'div',
                {
                  class: 'ah-view-stack',
                  style: {gap: '0.125rem'},
                },
                detail.pathFromRoot.map((pe, i) =>
                  m(
                    'div',
                    {
                      key: i,
                      class: `ah-path-entry${pe.isDominator ? ' ah-semibold' : ''}`,
                      style: {
                        paddingLeft: Math.min(i, 20) * 12,
                      },
                    },
                    [
                      m(
                        'span',
                        {class: 'ah-path-arrow'},
                        i === 0 ? '' : '\u2192',
                      ),
                      m(InstanceLink, {row: pe.row, navigate}),
                      pe.field
                        ? m('span', {class: 'ah-path-field'}, pe.field)
                        : null,
                    ],
                  ),
                ),
              ),
            )
          : null,

        m(Section, {title: 'Object Info'}, [
          m('div', {class: 'ah-info-grid'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Class:'),
            m(
              'span',
              detail.classObjRow
                ? m(InstanceLink, {
                    row: detail.classObjRow,
                    navigate,
                  })
                : '???',
            ),
            m('span', {class: 'ah-info-grid__label'}, 'Heap:'),
            m('span', row.heap),
            ...(row.isRoot
              ? [
                  m('span', {class: 'ah-info-grid__label'}, 'Root Types:'),
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
              },
              {
                metric: 'Retained',
                java: retainedJava,
                native: retainedNative,
              },
              {
                metric: 'Reachable',
                java: row.reachableSize,
                native: row.reachableNative,
              },
            ];
            return m(DataGrid, {
              schema: SIZE_SCHEMA,
              rootSchema: 'query',
              data: sizeRows,
              initialColumns: [
                {id: 'metric', field: 'metric'},
                {id: 'java', field: 'java'},
                {id: 'native', field: 'native'},
              ],
            });
          })(),
        ),

        detail.isClassObj
          ? m(Section, {title: 'Class Info'}, [
              m('div', {class: 'ah-info-grid ah-mb-3'}, [
                m('span', {class: 'ah-info-grid__label'}, 'Super Class:'),
                m(
                  'span',
                  detail.superClassObjId != null
                    ? m(InstanceLink, {
                        row: {
                          id: detail.superClassObjId,
                          display: fmtHex(detail.superClassObjId),
                        },
                        navigate,
                      })
                    : 'none',
                ),
                m('span', {class: 'ah-info-grid__label'}, 'Instance Size:'),
                m('span', {class: 'ah-mono'}, String(detail.instanceSize)),
              ]),
            ])
          : null,

        detail.isClassObj
          ? m(
              Section,
              {title: 'Static Fields'},
              renderFieldsGrid(detail.staticFields, navigate),
            )
          : null,

        detail.isClassInstance && detail.instanceFields.length > 0
          ? m(
              Section,
              {title: 'Fields'},
              renderFieldsGrid(detail.instanceFields, navigate),
            )
          : null,

        detail.isArrayInstance
          ? m(
              Section,
              {title: `Array Elements (${detail.arrayLength})`},
              renderArrayGrid(
                detail.arrayElems,
                detail.elemTypeName ?? 'Object',
                navigate,
                detail.elemTypeName === 'byte'
                  ? () => {
                      queries
                        .getRawArrayBlob(vnode.attrs.engine, params.id)
                        .then((blob) => {
                          if (blob !== null) {
                            downloadBlob(
                              `array-${fmtHex(params.id)}.bin`,
                              blob,
                            );
                          }
                        })
                        .catch(console.error);
                    }
                  : undefined,
              ),
            )
          : null,

        detail.reverseRefs.length > 0
          ? m(
              Section,
              {
                title: `Objects with References to this Object (${detail.reverseRefs.length})`,
                defaultOpen: detail.reverseRefs.length < 50,
              },
              m(
                'div',
                {class: 'ah-subtable'},
                m(DataGrid, {
                  schema: makeInstanceSchema(navigate),
                  rootSchema: 'query',
                  data: detail.reverseRefs.map(instanceRowToRow),
                  fillHeight: true,
                  initialColumns: [
                    {id: 'self_size', field: 'self_size'},
                    {id: 'native_size', field: 'native_size'},
                    {id: 'retained', field: 'retained'},
                    {id: 'retained_native', field: 'retained_native'},
                    {id: 'cls', field: 'cls'},
                    {id: 'id', field: 'id'},
                  ],
                  showExportButton: true,
                }),
              ),
            )
          : null,

        detail.dominated.length > 0
          ? m(
              Section,
              {
                title: `Immediately Dominated Objects (${detail.dominated.length})`,
                defaultOpen: detail.dominated.length < 50,
              },
              m(
                'div',
                {class: 'ah-subtable'},
                m(DataGrid, {
                  schema: makeInstanceSchema(navigate),
                  rootSchema: 'query',
                  data: detail.dominated.map(instanceRowToRow),
                  fillHeight: true,
                  initialColumns: [
                    {id: 'self_size', field: 'self_size'},
                    {id: 'native_size', field: 'native_size'},
                    {id: 'retained', field: 'retained'},
                    {id: 'retained_native', field: 'retained_native'},
                    {id: 'heap', field: 'heap'},
                    {id: 'cls', field: 'cls'},
                    {id: 'id', field: 'id'},
                  ],
                  showExportButton: true,
                }),
              ),
            )
          : null,
      ]);
    },
  };
}

// ─── Fields DataGrid ──────────────────────────────────────────────────────────

function renderFieldsGrid(fields: FieldRow[], navigate: NavFn): m.Children {
  if (fields.length === 0) {
    return m('div', {class: 'ah-info-grid__label'}, 'No fields');
  }
  return m(
    'div',
    {class: 'ah-subtable'},
    m(DataGrid, {
      schema: makeFieldSchema(navigate),
      rootSchema: 'query',
      data: fields.map(fieldRowToRow),
      fillHeight: true,
      initialColumns: [
        {id: 'type_name', field: 'type_name'},
        {id: 'name', field: 'name'},
        {id: 'value_display', field: 'value_display'},
        {id: 'shallow', field: 'shallow'},
        {id: 'shallow_native', field: 'shallow_native'},
        {id: 'retained', field: 'retained'},
        {id: 'retained_native', field: 'retained_native'},
      ],
      showExportButton: true,
    }),
  );
}

// ─── Array DataGrid ───────────────────────────────────────────────────────────

function renderArrayGrid(
  elems: ArrayElemRow[],
  elemTypeName: string,
  navigate: NavFn,
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
      ? m('div', {class: 'ah-mb-2', style: {display: 'flex', gap: '0.75rem'}}, [
          onDownloadBytes
            ? m(
                'button',
                {class: 'ah-download-link', onclick: onDownloadBytes},
                'Download bytes',
              )
            : null,
          elems.length > 0
            ? m(
                'button',
                {class: 'ah-download-link', onclick: copyTsv},
                'Copy as TSV',
              )
            : null,
        ])
      : null,
    m(
      'div',
      {class: 'ah-subtable'},
      m(DataGrid, {
        schema: makeArraySchema(navigate, elemTypeName),
        rootSchema: 'query',
        data: elems.map((e) => arrayElemToRow(e, elemTypeName)),
        fillHeight: true,
        initialColumns: [
          {id: 'idx', field: 'idx'},
          {id: 'value_display', field: 'value_display'},
          {id: 'shallow', field: 'shallow'},
          {id: 'shallow_native', field: 'shallow_native'},
          {id: 'retained', field: 'retained'},
          {id: 'retained_native', field: 'retained_native'},
        ],
        showExportButton: true,
      }),
    ),
  ]);
}

export default ObjectView;
