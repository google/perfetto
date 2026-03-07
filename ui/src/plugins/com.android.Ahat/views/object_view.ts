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
import {Spinner} from '../../../widgets/spinner';
import type {InstanceRow, InstanceDetail, HeapInfo, PrimOrRef} from '../types';
import {fmtSize, fmtHex} from '../format';
import {downloadBlob} from '../download';
import {
  type NavFn,
  InstanceLink,
  Section,
  SortableTable,
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
}

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
      const {heaps, navigate, params} = vnode.attrs;

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

      return m('div', {class: 'ah-view-stack'}, [
        m('div', [
          m(
            'h2',
            {
              class: 'ah-view-heading',
              style: {marginBottom: '0.25rem'},
            },
            'Object ' + fmtHex(row.id),
          ),
          m('div', m(InstanceLink, {row, navigate})),
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

        m(Section, {title: 'Object Size'}, [
          m('div', {class: 'ah-info-grid'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Shallow Size:'),
            m('span', {class: 'ah-mono'}, [
              fmtSize(row.shallowJava + row.shallowNative),
              row.shallowNative > 0
                ? m(
                    'span',
                    {style: {color: 'var(--ah-text-faint)'}},
                    ' (java: ' +
                      fmtSize(row.shallowJava) +
                      ', native: ' +
                      fmtSize(row.shallowNative) +
                      ')',
                  )
                : null,
            ]),
            m('span', {class: 'ah-info-grid__label'}, 'Retained Size:'),
            m(
              'span',
              {class: 'ah-mono ah-semibold'},
              fmtSize(row.retainedTotal),
            ),
          ]),
        ]),

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
              m(FieldsTable, {
                fields: detail.staticFields,
                navigate,
              }),
            )
          : null,

        detail.isClassInstance && detail.instanceFields.length > 0
          ? m(
              Section,
              {title: 'Fields'},
              m(FieldsTable, {
                fields: detail.instanceFields,
                navigate,
              }),
            )
          : null,

        detail.isArrayInstance
          ? m(
              Section,
              {title: `Array Elements (${detail.arrayLength})`},
              m(ArrayView, {
                elems: detail.arrayElems,
                elemTypeName: detail.elemTypeName ?? 'Object',
                total: detail.arrayLength,
                navigate,
                onDownloadBytes:
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
              }),
            )
          : null,

        detail.reverseRefs.length > 0
          ? m(
              Section,
              {
                title: `Objects with References to this Object (${detail.reverseRefs.length})`,
                defaultOpen: detail.reverseRefs.length < 50,
              },
              m(SortableTable, {
                columns: [
                  {
                    label: 'Object',
                    render: (r: InstanceRow) =>
                      m(InstanceLink, {row: r, navigate}),
                  },
                ],
                data: detail.reverseRefs,
                rowKey: (r: InstanceRow) => r.id,
              }),
            )
          : null,

        detail.dominated.length > 0
          ? m(
              Section,
              {
                title: `Immediately Dominated Objects (${detail.dominated.length})`,
                defaultOpen: detail.dominated.length < 50,
              },
              m(SortableTable, {
                columns: [
                  {
                    label: 'Retained',
                    align: 'right',
                    sortKey: (r: InstanceRow) => r.retainedTotal,
                    render: (r: InstanceRow) =>
                      m('span', {class: 'ah-mono'}, fmtSize(r.retainedTotal)),
                  },
                  ...heaps
                    .filter((h) => h.java + h.native_ > 0)
                    .map((h) => ({
                      label: h.name,
                      align: 'right',
                      sortKey: (r: InstanceRow) => {
                        const s = r.retainedByHeap.find(
                          (x) => x.heap === h.name,
                        );
                        return (s?.java ?? 0) + (s?.native_ ?? 0);
                      },
                      render: (r: InstanceRow) => {
                        const s = r.retainedByHeap.find(
                          (x) => x.heap === h.name,
                        );
                        return m(
                          'span',
                          {class: 'ah-mono'},
                          fmtSize((s?.java ?? 0) + (s?.native_ ?? 0)),
                        );
                      },
                    })),
                  {
                    label: 'Object',
                    render: (r: InstanceRow) =>
                      m(InstanceLink, {row: r, navigate}),
                  },
                ],
                data: detail.dominated,
                rowKey: (r: InstanceRow) => r.id,
              }),
            )
          : null,
      ]);
    },
  };
}

interface FieldsTableAttrs {
  fields: {name: string; typeName: string; value: PrimOrRef}[];
  navigate: NavFn;
}

function FieldsTable(): m.Component<FieldsTableAttrs> {
  return {
    view(vnode) {
      const {fields, navigate} = vnode.attrs;
      return m('div', {class: 'ah-table-wrap'}, [
        m('table', {style: {width: '100%'}}, [
          m('thead', [
            m('tr', [
              m('th', {class: 'ah-fields-th'}, 'Type'),
              m('th', {class: 'ah-fields-th'}, 'Name'),
              m('th', {class: 'ah-fields-th'}, 'Value'),
            ]),
          ]),
          m(
            'tbody',
            fields.map((f, i) =>
              m('tr', {key: i, class: 'ah-fields-tr'}, [
                m('td', {class: 'ah-fields-td--type'}, f.typeName),
                m('td', {class: 'ah-fields-td--name'}, f.name),
                m(
                  'td',
                  {class: 'ah-fields-td'},
                  m(PrimOrRefCell, {v: f.value, navigate}),
                ),
              ]),
            ),
          ),
        ]),
      ]);
    },
  };
}

const ARRAY_SHOW_LIMIT = 5_000;

interface ArrayViewAttrs {
  elems: {idx: number; value: PrimOrRef}[];
  elemTypeName: string;
  total: number;
  navigate: NavFn;
  onDownloadBytes?: () => void;
}

function ArrayView(): m.Component<ArrayViewAttrs> {
  let showCount = ARRAY_SHOW_LIMIT;

  return {
    view(vnode) {
      const {elems, elemTypeName, navigate, onDownloadBytes} = vnode.attrs;
      const visible = elems.slice(0, showCount);

      function copyTsv() {
        const header = 'Index\tValue';
        const lines = elems.map(
          (e) =>
            e.idx +
            '\t' +
            (e.value.kind === 'prim' ? e.value.v : e.value.display),
        );
        navigator.clipboard
          .writeText(header + '\n' + lines.join('\n'))
          .catch(console.error);
      }

      return m('div', [
        onDownloadBytes || elems.length > 0
          ? m(
              'div',
              {class: 'ah-mb-2', style: {display: 'flex', gap: '0.75rem'}},
              [
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
              ],
            )
          : null,
        m('table', {style: {width: '100%'}}, [
          m('thead', [
            m('tr', [
              m(
                'th',
                {
                  class: 'ah-fields-th',
                  style: {textAlign: 'right', width: '4rem'},
                },
                'Index',
              ),
              m('th', {class: 'ah-fields-th'}, 'Value (' + elemTypeName + ')'),
            ]),
          ]),
          m(
            'tbody',
            visible.map((e) =>
              m('tr', {key: e.idx, class: 'ah-fields-tr'}, [
                m('td', {class: 'ah-fields-td--index'}, String(e.idx)),
                m(
                  'td',
                  {class: 'ah-fields-td'},
                  m(PrimOrRefCell, {v: e.value, navigate}),
                ),
              ]),
            ),
          ),
        ]),
        elems.length > showCount
          ? m('div', {class: 'ah-table__more'}, [
              'Showing ' +
                showCount.toLocaleString() +
                ' of ' +
                elems.length.toLocaleString(),
              ' \u2014 ',
              m(
                'button',
                {
                  class: 'ah-more-link',
                  onclick: () => {
                    showCount = Math.min(showCount + 5_000, elems.length);
                  },
                },
                'show more',
              ),
              ' ',
              m(
                'button',
                {
                  class: 'ah-more-link',
                  onclick: () => {
                    showCount = elems.length;
                  },
                },
                'show all',
              ),
            ])
          : null,
        vnode.attrs.total > elems.length
          ? m(
              'div',
              {class: 'ah-table__more ah-mt-2'},
              'Showing first ' +
                elems.length.toLocaleString() +
                ' of ' +
                vnode.attrs.total.toLocaleString() +
                ' elements',
            )
          : null,
      ]);
    },
  };
}

export default ObjectView;
