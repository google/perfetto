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
import {Duration} from '../../../base/time';
import type {SqlValue, Row} from '../../../trace_processor/query_result';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import type {ColumnSchema} from '../../../components/widgets/datagrid/datagrid_schema';
import type {OverviewData} from '../types';
import {sizeRenderer, fmtSize} from '../components';
import {HdeAnchor} from '../nav';
import * as queries from '../queries';
import {AsyncMemo} from '../../../base/async_memo';
import {Spinner} from '../../../widgets/spinner';
import {
  Grid,
  GridCell,
  GridHeaderCell,
  type GridRow,
} from '../../../widgets/grid';
import {removeFalsyValues} from '../../../base/array_utils';
import {
  OOME_DETAILS_TITLE,
  renderOomeDetailsGrid,
} from '../../dev.perfetto.HeapProfile/oome_callstack_common';
import {DetailsShell} from '../../../widgets/details_shell';
import {HeapDumpExplorerSession} from '../session';

const HEAP_SCHEMA: ColumnSchema = {
  heap: {
    title: 'Heap',
    columnType: 'text',
  },
  java_size: {
    title: 'Java Size',
    columnType: 'quantitative',
    cellRenderer: sizeRenderer,
  },
  native_size: {
    title: 'Native Size',
    columnType: 'quantitative',
    cellRenderer: sizeRenderer,
  },
  total_size: {
    title: 'Total Size',
    columnType: 'quantitative',
    cellRenderer: sizeRenderer,
  },
};

function makeDuplicateBitmapSchema(dump: queries.HeapDump): ColumnSchema {
  return {
    dimensions: {
      title: 'Dimensions',
      columnType: 'text',
    },
    copies: {
      title: 'Copies',
      columnType: 'quantitative',
      cellRenderer: (value: SqlValue, row) =>
        m(
          HdeAnchor,
          {
            dump,
            to: {view: 'bitmaps', filterKey: String(row.groupKey ?? '')},
          },
          String(value),
        ),
    },
    total_bytes: {
      title: 'Total',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    wasted_bytes: {
      title: 'Wasted',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
  };
}

function makeDuplicateArraySchema(dump: queries.HeapDump): ColumnSchema {
  return {
    className: {
      title: 'Array Type',
      columnType: 'text',
      cellRenderer: (value: SqlValue) =>
        m(
          HdeAnchor,
          {dump, to: {view: 'objects', cls: String(value ?? '')}},
          String(value ?? ''),
        ),
    },
    arrayHash: {
      title: 'Hash',
      columnType: 'text',
    },
    copies: {
      title: 'Copies',
      columnType: 'quantitative',
      cellRenderer: (value: SqlValue, row) =>
        m(
          HdeAnchor,
          {
            dump,
            to: {view: 'arrays', arrayHash: String(row.arrayHash ?? '')},
          },
          String(value),
        ),
    },
    total_bytes: {
      title: 'Total',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    wasted_bytes: {
      title: 'Wasted',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
  };
}

function makeDuplicateStringSchema(dump: queries.HeapDump): ColumnSchema {
  return {
    value: {
      title: 'Value',
      columnType: 'text',
      cellRenderer: (value: SqlValue) =>
        m(
          HdeAnchor,
          {
            class: 'pf-hde-mono pf-hde-break-all pf-hde-str-color',
            dump,
            to: {view: 'strings', q: String(value ?? '')},
          },
          '"' +
            (String(value ?? '').length > 200
              ? String(value).slice(0, 200) + '\u2026'
              : String(value ?? '')) +
            '"',
        ),
    },
    copies: {
      title: 'Copies',
      columnType: 'quantitative',
      cellRenderer: (value: SqlValue, row) =>
        m(
          HdeAnchor,
          {dump, to: {view: 'strings', q: String(row.value ?? '')}},
          String(value),
        ),
    },
    total_bytes: {
      title: 'Total',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
    wasted_bytes: {
      title: 'Wasted',
      columnType: 'quantitative',
      cellRenderer: sizeRenderer,
    },
  };
}

function renderDuplicateSection(
  dump: queries.HeapDump,
  title: string,
  groupCount: number,
  totalWasted: number,
  targetView: 'bitmaps' | 'strings' | 'arrays',
  linkLabel: string,
  schema: ColumnSchema,
  data: Row[],
  columns: Array<{id: string; field: string}>,
): m.Children {
  return m('div', {class: 'pf-hde-card pf-hde-mt-4'}, [
    m('h3', {class: 'pf-hde-sub-heading'}, title),
    m('p', {class: 'pf-hde-desc'}, [
      groupCount +
        ' group' +
        (groupCount > 1 ? 's' : '') +
        ' detected, wasting ',
      m('span', {class: 'pf-hde-mono pf-hde-semibold'}, fmtSize(totalWasted)),
      '. ',
      m(HdeAnchor, {dump, to: {view: targetView}}, linkLabel),
    ]),
    m('div', {class: 'pf-hde-dup-grid-container'}, [
      m(DataGrid, {
        schema,
        data,
        initialColumns: columns,
        fillHeight: true,
      }),
    ]),
  ]);
}

interface OverviewViewAttrs {
  readonly session: HeapDumpExplorerSession;
  readonly dump: queries.HeapDump;
}
export function OverviewView(): m.Component<OverviewViewAttrs> {
  const overviewMemo = new AsyncMemo<OverviewData>();

  return {
    onremove() {
      overviewMemo.dispose();
    },
    view(vnode) {
      const {session, dump} = vnode.attrs;

      const {isPending, data: overview} = overviewMemo.use({
        key: {upid: dump.upid, ts: dump.ts},
        compute: () =>
          queries.getOverview(
            session.trace.engine,
            dump,
            session.hasFieldValues,
          ),
      });
      if (isPending) {
        return m(
          DetailsShell,
          {
            title: 'Overview',
            fillHeight: true,
            className: 'pf-hde-tab--padded',
          },
          m('.pf-hde-loading', m(Spinner, {easing: true})),
        );
      }

      const heapIndices: number[] = [];
      for (let i = 0; i < overview.heaps.length; i++) {
        const h = overview.heaps[i];
        if (h.java + h.native_ > 0) {
          heapIndices.push(i);
        }
      }
      const heaps = heapIndices.map((i) => overview.heaps[i]);
      const totalJava = heaps.reduce((a, h) => a + h.java, 0);
      const totalNative = heaps.reduce((a, h) => a + h.native_, 0);

      const heapRows: Row[] = [
        {
          heap: 'Total',
          java_size: totalJava,
          native_size: totalNative,
          total_size: totalJava + totalNative,
        },
        ...heaps.map((h) => ({
          heap: h.name,
          java_size: h.java,
          native_size: h.native_,
          total_size: h.java + h.native_,
        })),
      ];

      const processLabel =
        (dump.processName ?? '<unknown>') +
        (dump.pid ? ` (pid ${dump.pid})` : '');
      const infoRow = (property: string, value: string): GridRow => [
        m(GridCell, property),
        m(GridCell, value),
      ];

      return m(
        DetailsShell,
        {
          title: 'Overview',
          fillHeight: true,
          className: 'pf-hde-tab--padded',
        },
        [
          m('div', {class: 'pf-hde-card pf-hde-mb-4'}, [
            m('h3', {class: 'pf-hde-sub-heading'}, 'General Information'),
            m(Grid, {
              columns: [
                {key: 'property', header: m(GridHeaderCell, 'Property')},
                {key: 'value', header: m(GridHeaderCell, 'Value')},
              ],
              rowData: removeFalsyValues([
                infoRow('Process', processLabel),
                overview.processUptime !== null &&
                  infoRow('Uptime', Duration.format(overview.processUptime)),
                overview.oomBucket !== null &&
                  infoRow(
                    'OOM score',
                    `${overview.oomBucket} (${overview.oomScore})`,
                  ),
                infoRow('Classes', overview.classCount.toLocaleString()),
                infoRow(
                  'Reachable instances',
                  overview.reachableInstanceCount.toLocaleString(),
                ),
                infoRow(
                  'Unreachable instances',
                  overview.unreachableInstanceCount.toLocaleString(),
                ),
                overview.anonRssAndSwapSize !== null &&
                  infoRow(
                    'Anon RSS + Swap',
                    fmtSize(Number(overview.anonRssAndSwapSize)),
                  ),
                overview.dmabufRssSize !== null &&
                  infoRow(
                    'DMA Buffer RSS',
                    fmtSize(Number(overview.dmabufRssSize)),
                  ),
              ]),
            }),
          ]),
          m('div', {class: 'pf-hde-card'}, [
            m('h3', {class: 'pf-hde-sub-heading'}, 'Bytes Retained by Heap'),
            m(DataGrid, {
              schema: HEAP_SCHEMA,
              data: heapRows,
              initialColumns: [
                {id: 'heap', field: 'heap'},
                {id: 'java_size', field: 'java_size'},
                {id: 'native_size', field: 'native_size'},
                {id: 'total_size', field: 'total_size'},
              ],
            }),
          ]),
          overview.oome !== undefined
            ? m('div', {class: 'pf-hde-card pf-hde-mt-4'}, [
                m('h3', {class: 'pf-hde-sub-heading'}, OOME_DETAILS_TITLE),
                renderOomeDetailsGrid(overview.oome),
              ])
            : null,
          overview.duplicateBitmaps && overview.duplicateBitmaps.length > 0
            ? renderDuplicateSection(
                dump,
                'Duplicate Bitmaps',
                overview.duplicateBitmaps.length,
                overview.duplicateBitmaps.reduce(
                  (a, g) => a + g.wastedBytes,
                  0,
                ),
                'bitmaps',
                'View Bitmaps',
                makeDuplicateBitmapSchema(dump),
                overview.duplicateBitmaps.map((g) => ({
                  dimensions: `${g.width} \u00d7 ${g.height}`,
                  groupKey: g.groupKey,
                  copies: g.count,
                  total_bytes: g.totalBytes,
                  wasted_bytes: g.wastedBytes,
                })),
                [
                  {id: 'dimensions', field: 'dimensions'},
                  {id: 'groupKey', field: 'groupKey'},
                  {id: 'copies', field: 'copies'},
                  {id: 'total_bytes', field: 'total_bytes'},
                  {id: 'wasted_bytes', field: 'wasted_bytes'},
                ],
              )
            : session.hasFieldValues
              ? m(
                  'div',
                  {class: 'pf-hde-card pf-hde-mt-4 pf-hde-mb-4'},
                  m(
                    'p',
                    {class: 'pf-hde-muted'},
                    'No duplicate bitmaps found.',
                  ),
                )
              : null,
          overview.duplicateStrings && overview.duplicateStrings.length > 0
            ? renderDuplicateSection(
                dump,
                'Duplicate Strings',
                overview.duplicateStrings.length,
                overview.duplicateStrings.reduce(
                  (a, g) => a + g.wastedBytes,
                  0,
                ),
                'strings',
                'View Strings',
                makeDuplicateStringSchema(dump),
                overview.duplicateStrings.map((g) => ({
                  value: g.value,
                  copies: g.count,
                  total_bytes: g.totalBytes,
                  wasted_bytes: g.wastedBytes,
                })),
                [
                  {id: 'value', field: 'value'},
                  {id: 'copies', field: 'copies'},
                  {id: 'total_bytes', field: 'total_bytes'},
                  {id: 'wasted_bytes', field: 'wasted_bytes'},
                ],
              )
            : session.hasFieldValues
              ? m(
                  'div',
                  {class: 'pf-hde-card pf-hde-mb-4'},
                  m(
                    'p',
                    {class: 'pf-hde-muted'},
                    'No duplicate strings found.',
                  ),
                )
              : null,
          overview.duplicateArrays && overview.duplicateArrays.length > 0
            ? renderDuplicateSection(
                dump,
                'Duplicate Primitive Arrays',
                overview.duplicateArrays.length,
                overview.duplicateArrays.reduce((a, g) => a + g.wastedBytes, 0),
                'arrays',
                'View Arrays',
                makeDuplicateArraySchema(dump),
                overview.duplicateArrays.map((g) => ({
                  className: g.className,
                  arrayHash: g.arrayHash,
                  copies: g.count,
                  total_bytes: g.totalBytes,
                  wasted_bytes: g.wastedBytes,
                })),
                [
                  {id: 'className', field: 'className'},
                  {id: 'arrayHash', field: 'arrayHash'},
                  {id: 'copies', field: 'copies'},
                  {id: 'total_bytes', field: 'total_bytes'},
                  {id: 'wasted_bytes', field: 'wasted_bytes'},
                ],
              )
            : null,
        ],
      );
    },
  };
}
