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
import type {BitmapListRow, InstanceDetail} from '../types';
import {fmtSize, fmtHex} from '../format';
import {
  type NavFn,
  sizeRenderer,
  shortClassName,
  BitmapImage,
} from '../components';
import * as queries from '../queries';

// ─── DataGrid helpers for bitmap list ─────────────────────────────────────────

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
    heap: r.row.heap,
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

// ─── BitmapCard ──────────────────────────────────────────────────────────────

interface BitmapCardAttrs {
  row: BitmapListRow;
  engine: Engine;
  navigate: NavFn;
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
      );
    },
  };
}

// ─── BitmapGalleryView ───────────────────────────────────────────────────────

interface BitmapGalleryViewAttrs {
  engine: Engine;
  navigate: NavFn;
  hasFieldValues?: boolean;
}

function BitmapGalleryView(): m.Component<BitmapGalleryViewAttrs> {
  let rows: BitmapListRow[] | null = null;
  let alive = true;

  return {
    oninit(vnode) {
      queries
        .getBitmapList(vnode.attrs.engine)
        .then((r) => {
          if (!alive) return;
          rows = r;
          m.redraw();
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
          fillHeight: true,
        });
      }

      return m('div', {class: 'ah-view-scroll'}, [
        m('h2', {class: 'ah-view-heading'}, 'Bitmaps'),
        m('div', {class: 'ah-card ah-mb-4'}, [
          m('div', {class: 'ah-info-grid'}, [
            m('span', {class: 'ah-info-grid__label'}, 'Total bitmaps:'),
            m('span', {class: 'ah-mono'}, String(rows.length)),
            ...(withPixels.length > 0
              ? [
                  m('span', {class: 'ah-info-grid__label'}, 'With pixel data:'),
                  m('span', {class: 'ah-mono'}, String(withPixels.length)),
                ]
              : []),
            m('span', {class: 'ah-info-grid__label'}, 'Total retained:'),
            m('span', {class: 'ah-mono'}, fmtSize(totalRetained)),
          ]),
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
                }),
              ),
            )
          : null,
        withoutPixels.length > 0
          ? [
              m(
                'h3',
                {class: 'ah-muted-heading'},
                `${withoutPixels.length} bitmap${withoutPixels.length > 1 ? 's' : ''} without pixel data`,
              ),
              m(
                'div',
                {class: 'ah-subtable'},
                m(DataGrid, {
                  schema: makeBitmapListSchema(navigate),
                  rootSchema: 'query',
                  data: withoutPixels.map(bitmapRowToRow),
                  fillHeight: true,
                  initialColumns: [
                    {id: 'dimensions', field: 'dimensions'},
                    {id: 'self_size', field: 'self_size'},
                    {id: 'native_size', field: 'native_size'},
                    {id: 'retained', field: 'retained'},
                    {id: 'retained_native', field: 'retained_native'},
                    {id: 'id', field: 'id'},
                  ],
                  showExportButton: true,
                }),
              ),
            ]
          : null,
      ]);
    },
  };
}

export default BitmapGalleryView;
