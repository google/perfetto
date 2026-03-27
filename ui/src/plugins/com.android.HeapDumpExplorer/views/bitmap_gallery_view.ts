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
import {EmptyState} from '../../../widgets/empty_state';
import {DataGrid} from '../../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import type {Filter} from '../../../components/widgets/datagrid/model';
import type {BitmapListRow, InstanceDetail} from '../types';
import {fmtSize, fmtHex} from '../format';
import {
  type NavFn,
  sizeRenderer,
  countRenderer,
  shortClassName,
  BitmapImage,
  renderPath,
} from '../components';
import type {PathEntry} from '../types';
import * as queries from '../queries';

const SUMMARY_SCHEMA: SchemaRegistry = {
  query: {
    property: {title: 'Property', columnType: 'text'},
    value: {title: 'Value', columnType: 'text'},
  },
};

function bitmapRowToRow(r: BitmapListRow): Row {
  let retained = 0;
  let retainedNative = 0;
  for (const h of r.row.retainedByHeap) {
    retained += h.java;
    retainedNative += h.native_;
  }
  return {
    id: r.row.id,
    cls: r.row.className,
    dimensions: `${r.width}\u00d7${r.height}`,
    pixel_count: r.pixelCount,
    self_size: r.row.shallowJava,
    native_size: r.row.shallowNative,
    retained,
    retained_native: retainedNative,
    retained_count: r.row.retainedCount,
    reachable_size: r.row.reachableSize,
    reachable_native: r.row.reachableNative,
    reachable_count: r.row.reachableCount,
    heap: r.row.heap,
    buffer_hash: r.bufferHash,
  };
}

function makeBitmapListSchema(navigate: NavFn): SchemaRegistry {
  return {
    query: {
      id: {
        title: 'Object',
        columnType: 'identifier',
        cellRenderer: (value: SqlValue, row) => {
          const id = Number(value);
          const cls = String(row.cls ?? '');
          const display = `${shortClassName(cls)} ${fmtHex(id)}`;
          return m(
            'button',
            {
              class: 'ah-link',
              onclick: () =>
                navigate('object', {
                  id,
                  label: `Bitmap ${row.dimensions}`,
                }),
            },
            display,
          );
        },
      },
      dimensions: {
        title: 'Dimensions',
        columnType: 'text',
        cellRenderer: (value: SqlValue) =>
          m('span', {class: 'ah-mono'}, String(value ?? '')),
      },
      self_size: {
        title: 'Shallow',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      native_size: {
        title: 'Native',
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
      retained_count: {
        title: 'Retained #',
        columnType: 'quantitative',
        cellRenderer: countRenderer,
      },
      reachable_size: {
        title: 'Reachable',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_native: {
        title: 'Reachable Native',
        columnType: 'quantitative',
        cellRenderer: sizeRenderer,
      },
      reachable_count: {
        title: 'Reachable #',
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
      pixel_count: {
        title: 'Pixels',
        columnType: 'quantitative',
      },
    },
  };
}

interface BitmapCardAttrs {
  readonly row: BitmapListRow;
  readonly engine: Engine;
  readonly navigate: NavFn;
  readonly pathData?: PathEntry[] | null;
}

function BitmapCard(): m.Component<BitmapCardAttrs> {
  let obs: IntersectionObserver | null = null;
  let bitmap: InstanceDetail['bitmap'] | null | 'loading' | 'error' = null;

  function load(engine: Engine, id: number) {
    if (bitmap !== null) return;
    bitmap = 'loading';
    queries
      .getBitmapPixels(engine, id)
      .then((result) => {
        bitmap = result ?? 'error';
        m.redraw();
      })
      .catch(() => {
        bitmap = 'error';
        m.redraw();
      });
  }

  return {
    oncreate(vnode) {
      if (!vnode.attrs.row.hasPixelData) return;
      obs = new IntersectionObserver(
        ([entry]) => {
          if (entry.isIntersecting) {
            load(vnode.attrs.engine, vnode.attrs.row.row.id);
            obs!.disconnect();
          }
        },
        {rootMargin: '400px'},
      );
      obs.observe(vnode.dom as Element);
    },
    onremove() {
      obs?.disconnect();
    },
    view(vnode) {
      const {row, navigate} = vnode.attrs;
      const dpi = row.density > 0 ? row.density : 420;
      const scale = dpi / 160;
      const dpW = Math.round(row.width / scale);
      const dpH = Math.round(row.height / scale);

      const imageArea =
        bitmap !== null && typeof bitmap === 'object'
          ? m(BitmapImage, {
              width: bitmap.width,
              height: bitmap.height,
              format: bitmap.format,
              data: bitmap.data,
            })
          : bitmap === 'loading'
            ? m('span', {class: 'ah-bitmap-card__secondary'}, '\u2026')
            : bitmap === 'error'
              ? m('span', {class: 'ah-bitmap-card__secondary'}, 'no data')
              : !row.hasPixelData
                ? m(
                    'span',
                    {class: 'ah-bitmap-card__secondary'},
                    'no pixel data',
                  )
                : null;

      return m(
        'div',
        {class: 'ah-bitmap-card'},
        m(
          'div',
          {
            class: 'ah-bitmap-card__image',
            style: {
              maxWidth: `${dpW}px`,
              maxHeight: '45vh',
              aspectRatio: `${row.width} / ${row.height}`,
            },
          },
          imageArea,
        ),
        m(
          'div',
          {class: 'ah-bitmap-card__info'},
          m(
            'div',
            null,
            m('span', {class: 'ah-mono'}, `${row.width}\u00d7${row.height} px`),
            m(
              'span',
              {class: 'ah-bitmap-card__secondary'},
              `${dpW}\u00d7${dpH} dp`,
            ),
            m('span', {class: 'ah-bitmap-card__secondary'}, `@${dpi}dpi`),
            m(
              'span',
              {class: 'ah-bitmap-card__secondary'},
              fmtSize(row.row.retainedTotal),
            ),
          ),
          m(
            'button',
            {
              class: 'ah-link',
              onclick: () =>
                navigate('object', {
                  id: row.row.id,
                  label: `Bitmap ${row.width}\u00d7${row.height}`,
                }),
            },
            'Details',
          ),
        ),
        vnode.attrs.pathData !== undefined && vnode.attrs.pathData !== null
          ? m(
              'div',
              {class: 'ah-bitmap-card__path'},
              vnode.attrs.pathData.length > 0
                ? renderPath(vnode.attrs.pathData, navigate)
                : m('span', {class: 'ah-muted'}, 'No path to GC root.'),
            )
          : null,
      );
    },
  };
}

interface BitmapGalleryViewAttrs {
  readonly engine: Engine;
  readonly navigate: NavFn;
  readonly hasFieldValues?: boolean;
  readonly filterKey?: string;
}

function BitmapGalleryView(): m.Component<BitmapGalleryViewAttrs> {
  let rows: BitmapListRow[] | null = null;
  let alive = true;
  let showPaths = false;
  let pathsFetched = false;
  const pathMap = new Map<number, PathEntry[]>();

  function fetchAllPaths(engine: Engine, bitmaps: BitmapListRow[]) {
    const ids = bitmaps.map((b) => b.row.id);
    if (ids.length === 0) return;
    queries
      .fetchPathsFromRoot(engine, ids)
      .then((paths) => {
        if (!alive) return;
        for (const id of ids) {
          pathMap.set(id, paths.get(id) ?? []);
        }
        pathsFetched = true;
        m.redraw();
      })
      .catch(console.error);
  }

  return {
    oninit(vnode) {
      queries
        .getBitmapList(vnode.attrs.engine)
        .then((r) => {
          if (!alive) return;
          rows = r;
          m.redraw();
          // Enrich with reachable sizes asynchronously.
          queries
            .enrichWithReachable(
              vnode.attrs.engine,
              r.map((b) => b.row),
            )
            .then(() => {
              if (alive) m.redraw();
            })
            .catch(console.error);
        })
        .catch(console.error);
    },
    onremove() {
      alive = false;
    },
    view(vnode) {
      const {engine, navigate} = vnode.attrs;

      if (!rows) {
        return m('div', {class: 'ah-loading'}, m(Spinner, {easing: true}));
      }

      const totalRetained = rows.reduce(
        (sum, r) => sum + r.row.retainedTotal,
        0,
      );
      const withPixels = rows.filter((r) => r.hasPixelData);
      const withoutPixels = rows.filter((r) => !r.hasPixelData);

      if (vnode.attrs.hasFieldValues === false || rows.length === 0) {
        return m(EmptyState, {
          icon: 'image',
          title:
            vnode.attrs.hasFieldValues === false
              ? 'Bitmap data requires an ART heap dump (.hprof)'
              : 'No bitmap data available',
          className: 'ah-empty-fill',
        });
      }

      const hashFilter: Filter[] = vnode.attrs.filterKey
        ? [
            {
              field: 'buffer_hash',
              op: '=' as const,
              value: vnode.attrs.filterKey,
            },
          ]
        : [];
      const bitmapSchema = makeBitmapListSchema(navigate);
      const bitmapColumns = [
        {id: 'dimensions', field: 'dimensions'},
        {id: 'self_size', field: 'self_size'},
        {id: 'native_size', field: 'native_size'},
        {id: 'retained', field: 'retained'},
        {id: 'retained_native', field: 'retained_native'},
        {id: 'retained_count', field: 'retained_count'},
        {id: 'reachable_size', field: 'reachable_size'},
        {id: 'reachable_native', field: 'reachable_native'},
        {id: 'reachable_count', field: 'reachable_count'},
        {id: 'id', field: 'id'},
        {id: 'cls', field: 'cls'},
        {id: 'buffer_hash', field: 'buffer_hash'},
      ];

      return m('div', {class: 'ah-view-scroll'}, [
        m('div', {class: 'ah-heading-row'}, [
          m(
            'h2',
            {class: 'ah-view-heading'},
            `Bitmaps (${rows.length.toLocaleString()})`,
          ),
          m(
            'button',
            {
              class: 'ah-link--alt',
              onclick: () => {
                showPaths = !showPaths;
                if (showPaths && !pathsFetched) {
                  fetchAllPaths(engine, rows!);
                }
              },
            },
            showPaths ? 'Hide Paths' : 'Show Paths',
          ),
        ]),
        m('div', {class: 'ah-card ah-mb-4'}, [
          m(DataGrid, {
            schema: SUMMARY_SCHEMA,
            rootSchema: 'query',
            data: [
              {property: 'Total bitmaps', value: String(rows.length)},
              ...(withPixels.length > 0
                ? [
                    {
                      property: 'With pixel data',
                      value: String(withPixels.length),
                    },
                  ]
                : []),
              {property: 'Total retained', value: fmtSize(totalRetained)},
            ],
            initialColumns: [
              {id: 'property', field: 'property'},
              {id: 'value', field: 'value'},
            ],
          }),
        ]),
        withPixels.length > 0
          ? m(
              'div',
              {class: 'ah-mb-4'},
              withPixels.map((r) =>
                m(BitmapCard, {
                  key: r.row.id,
                  row: r,
                  engine,
                  navigate,
                  pathData: showPaths ? pathMap.get(r.row.id) ?? null : null,
                }),
              ),
            )
          : null,
        withPixels.length > 0
          ? m('div', {class: 'ah-mb-4'}, [
              m(
                'h3',
                {class: 'ah-muted-heading'},
                `${withPixels.length} bitmap${withPixels.length > 1 ? 's' : ''} with pixel data`,
              ),
              m(DataGrid, {
                schema: bitmapSchema,
                rootSchema: 'query',
                data: withPixels.map(bitmapRowToRow),
                initialColumns: bitmapColumns,
                ...(hashFilter.length > 0 ? {initialFilters: hashFilter} : {}),
                showExportButton: true,
              }),
            ])
          : null,
        withoutPixels.length > 0
          ? m('div', {class: 'ah-mb-4'}, [
              m(
                'h3',
                {class: 'ah-muted-heading ah-mt-4'},
                `${withoutPixels.length} bitmap${withoutPixels.length > 1 ? 's' : ''} without pixel data`,
              ),
              m(DataGrid, {
                schema: bitmapSchema,
                rootSchema: 'query',
                data: withoutPixels.map(bitmapRowToRow),
                initialColumns: bitmapColumns,
                ...(hashFilter.length > 0 ? {initialFilters: hashFilter} : {}),
                showExportButton: true,
              }),
            ])
          : null,
      ]);
    },
  };
}

export default BitmapGalleryView;
