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
import type {CellRenderResult} from '../../components/widgets/datagrid/datagrid_schema';
import type {InstanceRow, PrimOrRef} from './types';
import {fmtSize} from './format';
import type {BreadcrumbEntry, NavState} from './nav_state';
export type {BreadcrumbEntry};

export type NavFn = (
  view: NavState['view'],
  params?: Record<string, unknown>,
) => void;

export type ObjLinkRef = {
  id: number;
  display: string;
  str?: string | null;
};

// ─── InstanceLink ─────────────────────────────────────────────────────────────

interface InstanceLinkAttrs {
  row: InstanceRow | ObjLinkRef | null;
  navigate: NavFn;
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

// ─── Section ──────────────────────────────────────────────────────────────────

interface SectionAttrs {
  title: string;
  defaultOpen?: boolean;
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

// ─── DataGrid cell renderers (shared across views) ──────────────────────────

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

/** Returns the short (unqualified) class name, preserving array brackets. */
export function shortClassName(full: string): string {
  const bracket = full.indexOf('[');
  const base = bracket >= 0 ? full.slice(0, bracket) : full;
  const dot = base.lastIndexOf('.');
  const short = dot >= 0 ? base.slice(dot + 1) : base;
  return bracket >= 0 ? short + full.slice(bracket) : short;
}

/** Shared SQL preamble for views that use the dominator tree module. */
export const DOMINATOR_TREE_PREAMBLE =
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree';

/** SQL preamble that includes both dominator tree and object tree modules. */
export const REACHABLE_PREAMBLE =
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.dominator_tree;\n' +
  'INCLUDE PERFETTO MODULE android.memory.heap_graph.object_tree';

// ─── ReachableToggle ──────────────────────────────────────────────────────────

interface ReachableToggleAttrs {
  checked: boolean;
  onchange: (checked: boolean) => void;
}

/** Checkbox to opt-in to expensive reachable size columns. */
export function ReachableToggle(): m.Component<ReachableToggleAttrs> {
  return {
    view(vnode) {
      const {checked, onchange} = vnode.attrs;
      return m('label', {class: 'ah-reachable-toggle'}, [
        m('input', {
          type: 'checkbox',
          checked,
          onchange: (e: Event) =>
            onchange((e.target as HTMLInputElement).checked),
        }),
        ' Show reachable',
      ]);
    },
  };
}

// ─── PrimOrRefCell ────────────────────────────────────────────────────────────

interface PrimOrRefCellAttrs {
  v: PrimOrRef;
  navigate: NavFn;
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

// ─── BitmapImage ──────────────────────────────────────────────────────────────

interface BitmapImageAttrs {
  width: number;
  height: number;
  format: string;
  data: Uint8Array;
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
      const imgStyle = {
        maxWidth: '100%',
        maxHeight: '100%',
        objectFit: 'contain' as const,
        imageRendering: 'pixelated' as const,
      };
      if (format === 'rgba') {
        return m('canvas', {style: imgStyle});
      }
      // Always render the img element so oncreate fires and creates the blob URL.
      // Before the blob URL is ready, src is empty (blank image).
      return m('img', {src: blobUrl ?? '', style: imgStyle});
    },
  };
}

// ─── Breadcrumbs ──────────────────────────────────────────────────────────────

interface BreadcrumbsAttrs {
  trail: BreadcrumbEntry[];
  activeIndex: number;
  onNavigate: (index: number) => void;
}
export function Breadcrumbs(): m.Component<BreadcrumbsAttrs> {
  return {
    view(vnode) {
      const {trail, activeIndex, onNavigate} = vnode.attrs;
      if (trail.length <= 1) return null;
      return m(
        'nav',
        {'class': 'ah-breadcrumbs', 'aria-label': 'Breadcrumb'},
        m(
          'button',
          {
            'class': 'ah-breadcrumbs__back',
            'onclick': () => {
              if (activeIndex > 0) onNavigate(activeIndex - 1);
            },
            'title': 'Back',
            'aria-label': 'Back',
          },
          m(
            'svg',
            {
              class: 'ah-breadcrumbs__back-icon',
              viewBox: '0 0 20 20',
              fill: 'currentColor',
            },
            m('path', {
              'fill-rule': 'evenodd',
              'd': 'M12.79 5.23a.75.75 0 01-.02 1.06L8.832 10l3.938 3.71a.75.75 0 11-1.04 1.08l-4.5-4.25a.75.75 0 010-1.08l4.5-4.25a.75.75 0 011.06.02z',
              'clip-rule': 'evenodd',
            }),
          ),
        ),
        trail.map((crumb, i) => {
          const isActive = i === activeIndex;
          return m(
            'span',
            {key: i, class: 'ah-breadcrumbs__item'},
            i > 0 ? m('span', {class: 'ah-breadcrumbs__sep'}, '/') : null,
            isActive
              ? m('span', {class: 'ah-breadcrumbs__active'}, crumb.label)
              : m(
                  'button',
                  {
                    class: `ah-breadcrumbs__link ${i > activeIndex ? 'ah-breadcrumbs__link--future' : 'ah-breadcrumbs__link--past'}`,
                    onclick: () => onNavigate(i),
                  },
                  crumb.label,
                ),
          );
        }),
      );
    },
  };
}
