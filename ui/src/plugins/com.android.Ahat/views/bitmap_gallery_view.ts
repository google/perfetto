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
import {EmptyState} from '../../../widgets/empty_state';
import type {BitmapListRow, InstanceDetail} from '../types';
import {fmtSize} from '../format';
import {
  type NavFn,
  InstanceLink,
  SortableTable,
  BitmapImage,
} from '../components';
import * as queries from '../queries';

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

      return m('div', [
        m('h2', {class: 'ah-view-heading'}, 'Bitmaps'),

        rows.length === 0
          ? m(EmptyState, {icon: 'image', title: 'No bitmap data available'})
          : [
              m('div', {class: 'ah-card ah-mb-4'}, [
                m('div', {class: 'ah-info-grid'}, [
                  m('span', {class: 'ah-info-grid__label'}, 'Total bitmaps:'),
                  m('span', {class: 'ah-mono'}, String(rows.length)),
                  m('span', {class: 'ah-info-grid__label'}, 'With pixel data:'),
                  m('span', {class: 'ah-mono'}, String(withPixels.length)),
                  m('span', {class: 'ah-info-grid__label'}, 'Total retained:'),
                  m('span', {class: 'ah-mono'}, fmtSize(totalRetained)),
                ]),
              ]),

              // Bitmap image cards (lazy-loaded)
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

              // Table for bitmaps without pixel data
              withoutPixels.length > 0
                ? [
                    m(
                      'h3',
                      {class: 'ah-muted-heading'},
                      `${withoutPixels.length} bitmap${withoutPixels.length > 1 ? 's' : ''} without pixel data`,
                    ),
                    m(SortableTable, {
                      columns: [
                        {
                          label: 'Size',
                          render: (r: BitmapListRow) =>
                            m(
                              'span',
                              {class: 'ah-mono'},
                              r.width + '\u00d7' + r.height,
                            ),
                        },
                        {
                          label: 'Density',
                          align: 'right' as const,
                          sortKey: (r: BitmapListRow) => r.density,
                          render: (r: BitmapListRow) =>
                            m(
                              'span',
                              {class: 'ah-mono'},
                              r.density > 0 ? String(r.density) : '\u2014',
                            ),
                        },
                        {
                          label: 'Retained',
                          align: 'right' as const,
                          sortKey: (r: BitmapListRow) => r.row.retainedTotal,
                          render: (r: BitmapListRow) =>
                            m(
                              'span',
                              {class: 'ah-mono'},
                              fmtSize(r.row.retainedTotal),
                            ),
                        },
                        {
                          label: 'Object',
                          render: (r: BitmapListRow) =>
                            m(InstanceLink, {
                              row: r.row,
                              navigate,
                            }),
                        },
                      ],
                      data: withoutPixels,
                      rowKey: (r: BitmapListRow) => r.row.id,
                    }),
                  ]
                : null,
            ],
      ]);
    },
  };
}

export default BitmapGalleryView;
