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
import type {SqlValue} from '../../trace_processor/query_result';
import {NUM} from '../../trace_processor/query_result';
import type {CellRenderResult} from '../../components/widgets/datagrid/datagrid_schema';
import type {Filter} from '../../components/widgets/datagrid/model';
import {filterToSql} from '../../components/widgets/datagrid/sql_utils';
import type {Engine} from '../../trace_processor/engine';
import type {InstanceRow, PrimOrRef} from './types';
import {fmtSize} from './format';
import type {NavState} from './nav_state';

export type NavFn = (
  view: NavState['view'],
  params?: Record<string, unknown>,
) => void;

export type ObjLinkRef = {
  id: number;
  display: string;
  str?: string | null;
};

interface InstanceLinkAttrs {
  readonly row: InstanceRow | ObjLinkRef | null;
  readonly navigate: NavFn;
}
export function InstanceLink(): m.Component<InstanceLinkAttrs> {
  return {
    view(vnode) {
      const {row, navigate} = vnode.attrs;
      if (!row || row.id === 0) {
        return m('span', {class: 'ah-badge-referent'}, 'ROOT');
      }
      const full = 'className' in row ? (row as InstanceRow) : null;
      return m(
        'span',
        full &&
          full.reachabilityName !== 'unreachable' &&
          full.reachabilityName !== 'strong'
          ? m('span', {class: 'ah-badge-reachability'}, full.reachabilityName)
          : null,
        full?.isRoot ? m('span', {class: 'ah-badge-root'}, 'root') : null,
        m(
          'button',
          {
            class: 'ah-link',
            onclick: () => navigate('object', {id: row.id, label: row.display}),
          },
          row.display,
        ),
        row.str != null
          ? m(
              'span',
              {
                class: 'ah-badge-string',
                title: row.str.length > 80 ? row.str : undefined,
              },
              '"' +
                (row.str.length > 80
                  ? row.str.slice(0, 80) + '\u2026'
                  : row.str) +
                '"',
            )
          : null,
        full?.referent
          ? m(
              'span',
              {class: 'ah-badge-referent'},
              ' for ',
              m(InstanceLink, {
                row: full.referent,
                navigate,
              }),
            )
          : null,
      );
    },
  };
}

interface SectionAttrs {
  readonly title: string;
  readonly defaultOpen?: boolean;
}
export function Section(): m.Component<SectionAttrs> {
  let open = true;
  return {
    oninit(vnode) {
      open = vnode.attrs.defaultOpen !== false;
    },
    view(vnode) {
      return m(
        'div',
        {class: 'ah-section'},
        m(
          'button',
          {
            'class': 'ah-section__toggle',
            'onclick': () => {
              open = !open;
            },
            'aria-expanded': open,
          },
          m('span', {class: 'ah-section__title'}, vnode.attrs.title),
          m(
            'svg',
            {
              class: `ah-section__chevron${open ? ' ah-section__chevron--open' : ''}`,
              viewBox: '0 0 20 20',
              fill: 'currentColor',
            },
            m('path', {
              'fill-rule': 'evenodd',
              'd': 'M7.21 14.77a.75.75 0 01.02-1.06L11.168 10 7.23 6.29a.75.75 0 111.04-1.08l4.5 4.25a.75.75 0 010 1.08l-4.5 4.25a.75.75 0 01-1.06-.02z',
              'clip-rule': 'evenodd',
            }),
          ),
        ),
        open ? m('div', {class: 'ah-section__body'}, vnode.children) : null,
      );
    },
  };
}

/** Renders a size value as a right-aligned formatted byte string. */
export function sizeRenderer(value: SqlValue): CellRenderResult {
  return {
    content: m('span', {class: 'ah-mono'}, fmtSize(Number(value ?? 0))),
    align: 'right',
  };
}

/** Renders a numeric count value as a right-aligned locale string. */
export function countRenderer(value: SqlValue): CellRenderResult {
  return {
    content: m('span', {class: 'ah-mono'}, Number(value ?? 0).toLocaleString()),
    align: 'right',
  };
}

/**
 * Returns the short (unqualified) class name, preserving generics and array
 *  brackets. Each fully-qualified segment between `<>`, `,` delimiters is
 *  shortened independently so `java.util.Map<java.lang.String, int[]>` becomes
 *  `Map<String, int[]>`.
 */
export function shortClassName(full: string): string {
  const bracket = full.indexOf('[');
  const base = bracket >= 0 ? full.slice(0, bracket) : full;
  const suffix = bracket >= 0 ? full.slice(bracket) : '';
  // Shorten each qualified-name token; delimiters (<>,) are preserved.
  const short = base.replace(/[\w$.]+/g, (tok) => {
    const dot = tok.lastIndexOf('.');
    return dot >= 0 ? tok.slice(dot + 1) : tok;
  });
  return short + suffix;
}

/** SQL preamble that includes dominator tree and object tree modules. */
export const SQL_PREAMBLE =
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;\n' +
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.object_tree';

/**
 * Tracks total and filtered row counts for a SQL-backed DataGrid view.
 *  Call `init()` in oninit, pass `onFiltersChanged` to DataGrid, and read
 *  `heading()` for the formatted title.
 */
export class RowCounter {
  total: number | null = null;
  filtered: number | null = null;

  private engine: Engine | null = null;
  private baseQuery = '';
  private preamble = '';
  private currentFilters: readonly Filter[] = [];

  init(engine: Engine, query: string, preamble = '') {
    this.engine = engine;
    this.baseQuery = query;
    this.preamble = preamble;
    this.runCount();
  }

  /** Format a heading like "Objects (1,234)" or "Objects (42 / 1,234)". */
  heading(label: string): string {
    if (this.total === null) return label;
    if (
      this.filtered !== null &&
      this.currentFilters.length > 0 &&
      this.filtered !== this.total
    ) {
      return `${label} (${this.filtered.toLocaleString()} / ${this.total.toLocaleString()})`;
    }
    return `${label} (${this.total.toLocaleString()})`;
  }

  /** Pass this as the DataGrid `onFiltersChanged` callback. */
  readonly onFiltersChanged = (filters: readonly Filter[]) => {
    this.currentFilters = filters;
    this.runFilteredCount();
  };

  private runCount() {
    if (!this.engine) return;
    const prefix = this.preamble ? `${this.preamble};\n` : '';
    this.engine
      .query(`${prefix}SELECT COUNT(*) AS cnt FROM (${this.baseQuery})`)
      .then((r) => {
        this.total = r.firstRow({cnt: NUM}).cnt;
        m.redraw();
      })
      .catch(console.error);
  }

  private runFilteredCount() {
    if (!this.engine || this.currentFilters.length === 0) {
      this.filtered = null;
      m.redraw();
      return;
    }
    const where = this.currentFilters
      .map((f) => filterToSql(f, f.field))
      .join(' AND ');
    const prefix = this.preamble ? `${this.preamble};\n` : '';
    this.engine
      .query(
        `${prefix}SELECT COUNT(*) AS cnt FROM (${this.baseQuery}) WHERE ${where}`,
      )
      .then((r) => {
        this.filtered = r.firstRow({cnt: NUM}).cnt;
        m.redraw();
      })
      .catch(console.error);
  }
}

interface PrimOrRefCellAttrs {
  readonly v: PrimOrRef;
  readonly navigate: NavFn;
}
export function PrimOrRefCell(): m.Component<PrimOrRefCellAttrs> {
  return {
    view(vnode) {
      const {v, navigate} = vnode.attrs;
      if (v.kind === 'ref') {
        return m(InstanceLink, {
          row: {id: v.id, display: v.display, str: v.str},
          navigate,
        });
      }
      return m('span', {class: 'ah-mono'}, v.v);
    },
  };
}

interface BitmapImageAttrs {
  readonly width: number;
  readonly height: number;
  readonly format: string;
  readonly data: Uint8Array;
}

export function BitmapImage(): m.Component<BitmapImageAttrs> {
  let blobUrl: string | null = null;

  return {
    oncreate(vnode) {
      const {width, height, format, data} = vnode.attrs;
      if (format === 'rgba') {
        const canvas = vnode.dom as HTMLCanvasElement;
        canvas.width = width;
        canvas.height = height;
        const ctx = canvas.getContext('2d');
        if (!ctx) return;
        const clamped = new Uint8ClampedArray(data.length);
        clamped.set(data);
        ctx.putImageData(new ImageData(clamped, width, height), 0, 0);
        return;
      }
      const mimeMap: Record<string, string> = {
        png: 'image/png',
        jpeg: 'image/jpeg',
        webp: 'image/webp',
      };
      const copy = new Uint8Array(data.length);
      copy.set(data);
      const blob = new Blob([copy], {
        type: mimeMap[format] ?? 'image/png',
      });
      blobUrl = URL.createObjectURL(blob);
      m.redraw();
    },
    onremove() {
      if (blobUrl) {
        URL.revokeObjectURL(blobUrl);
        blobUrl = null;
      }
    },
    view(vnode) {
      const {format} = vnode.attrs;
      if (format === 'rgba') {
        return m('canvas', {class: 'ah-bitmap-image'});
      }
      // Always render the img element so oncreate fires and creates the blob URL.
      // Before the blob URL is ready, src is empty (blank image).
      return m('img', {src: blobUrl ?? '', class: 'ah-bitmap-image'});
    },
  };
}
