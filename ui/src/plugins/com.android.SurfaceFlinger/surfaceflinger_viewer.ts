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

// The SurfaceFlinger 3-pane viewer (Surface rects | Hierarchy | Properties),
// the layers viewer built from native Perfetto widgets
// (Section, Tree, Chip, DataGrid). Reads/writes the shared SurfaceFlingerSession.

import './surfaceflinger.scss';
import m from 'mithril';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import type {Column} from '../../components/widgets/datagrid/model';
import {Button} from '../../widgets/button';
import {Checkbox} from '../../widgets/checkbox';
import {Chip} from '../../widgets/chip';
import {Intent} from '../../widgets/common';
import {Section} from '../../widgets/section';
import {TextInput} from '../../widgets/text_input';
import {Tree, TreeNode} from '../../widgets/tree';
import type {SqlValue} from '../../trace_processor/query_result';
import {
  rectsOptionsFrom,
  renderSurfaceControls,
} from './surfaceflinger_controls';
import {curatedProperties} from './surfaceflinger_curated';
import type {SfArg, SfLayer} from './surfaceflinger_data';
import {SfRectsView} from './surfaceflinger_rects';
import type {SurfaceFlingerSession} from './surfaceflinger_session';

const PROTO_SCHEMA: SchemaRegistry = {
  root: {
    property: {title: 'Property', columnType: 'text'},
    value: {title: 'Value', columnType: 'text'},
    type: {title: 'Type', columnType: 'text'},
  },
};
const PROTO_COLUMNS: readonly Column[] = [
  {id: 'property', field: 'property'},
  {id: 'value', field: 'value'},
  {id: 'type', field: 'type'},
];

const CURATED_COLUMNS: readonly Column[] = [
  {id: 'property', field: 'property'},
  {id: 'value', field: 'value'},
];

// With diff on, an extra "Previous" column shows the prior snapshot's value for
// any property that changed (blank if unchanged) — filter it to see only diffs.
const PROTO_SCHEMA_DIFF: SchemaRegistry = {
  root: {
    property: {title: 'Property', columnType: 'text'},
    value: {title: 'Value', columnType: 'text'},
    previous: {title: 'Previous', columnType: 'text'},
    type: {title: 'Type', columnType: 'text'},
  },
};
const PROTO_COLUMNS_DIFF: readonly Column[] = [
  {id: 'property', field: 'property'},
  {id: 'value', field: 'value'},
  {id: 'previous', field: 'previous'},
  {id: 'type', field: 'type'},
];

// Curated grid sizing: it has no internal scroll, so its height is derived from
// the row count (header rows + value rows), capped so a huge set still scrolls.
const CURATED_ROW_PX = 27;
const CURATED_PAD_PX = 40;
const CURATED_MAX_PX = 460;

function chipsFor(l: SfLayer, dup: boolean, relZParent: boolean): m.Children[] {
  const out: m.Children[] = [];
  const chip = (label: string, intent: Intent, title: string) =>
    m(Chip, {label, intent, compact: true, rounded: true, title});
  if (l.isVisible) out.push(chip('V', Intent.Success, 'Visible'));
  if (l.hwcCompositionType === 1) {
    out.push(chip('GPU', Intent.Warning, 'Client/GPU composition'));
  } else if (l.hwcCompositionType >= 2) {
    out.push(chip('HWC', Intent.Primary, 'Hardware composer composition'));
  }
  if (l.zOrderRelativeOf > 0) {
    out.push(chip('RelZ', Intent.None, 'Z-ordered relative to another layer'));
  }
  if (relZParent) {
    out.push(
      chip(
        'RelZParent',
        Intent.None,
        'Another layer is Z-ordered relative to this one',
      ),
    );
  }
  if (l.isHiddenByPolicy) {
    out.push(chip('H', Intent.Danger, 'Hidden by policy'));
  }
  if (l.isMissingZParent) {
    out.push(chip('MissingZ', Intent.Danger, 'Missing relative-Z parent'));
  }
  if (l.isSpy) out.push(chip('Spy', Intent.None, 'Input spy window'));
  if (dup) out.push(chip('Dup', Intent.Warning, 'Duplicate layer id'));
  return out;
}

interface TreeItem {
  layer: SfLayer;
  children: TreeItem[];
  deleted?: boolean; // present in the previous snapshot only (diff)
}

// Whether a proto arg holds its (zero/empty/false) default, used to hide noise
// when "Show defaults" is off. Only the empty string counts as a string default
// (a literal "0" string is a real value).
function isDefaultish(a: SfArg): boolean {
  if (a.type === 'bool') return a.value === 'false';
  if (a.type === 'string') return a.value === '';
  return Number(a.value) === 0;
}

// "Simplify names": collapse a long dotted class name (a.b.c.d.z) to
// "a.b.(...).z", leaving everything else (including any #id) intact.
function simplify(name: string, on: boolean): string {
  if (!on) return name;
  const parts = name.split('.');
  if (parts.length > 3) {
    return `${parts[0]}.${parts[1]}.(…).${parts[parts.length - 1]}`;
  }
  return name;
}

export interface SfViewerAttrs {
  session: SurfaceFlingerSession;
}

export class SfViewer implements m.ClassComponent<SfViewerAttrs> {
  // Memoized DataGrid sources, rebuilt only when their inputs change (by
  // reference), so the grids keep their internal filter/sort caches across the
  // many redraws from hovering and scrubbing.
  private curatedCache?: {
    layer: SfLayer;
    args: SfArg[];
    ds: InMemoryDataSource;
    schema: SchemaRegistry;
    count: number;
  };
  private protoCache?: {
    args: SfArg[];
    showDefaults: boolean;
    diff: boolean;
    prev: Map<string, string>;
    ds: InMemoryDataSource;
    count: number;
  };

  view(vnode: m.Vnode<SfViewerAttrs>): m.Children {
    const s = vnode.attrs.session;
    return m('.pf-sf-viewer', [
      this.renderSurface(s),
      this.renderHierarchy(s),
      this.renderProperties(s),
    ]);
  }

  // ---- Surface (rects) ----
  private renderSurface(s: SurfaceFlingerSession): m.Children {
    const o = s.options;
    return m('.pf-sf-pane.pf-sf-pane--surface', [
      m(Section, {title: 'Surface'}, [
        renderSurfaceControls(o),
        // Shows the selected display's composition (its group). The rects view
        // stays mounted across snapshot changes (keeping the previous frame
        // until the next loads) so scrubbing doesn't flicker the canvas. The
        // hierarchy and properties below use the full cross-display layer tree.
        m(SfRectsView, {
          layers: s.displayLayers(),
          selectedRowId: s.selectedRowId,
          hiddenLayerIds: s.hiddenLayerIds,
          pinnedLayerIds: s.pinnedLayerIds,
          onSelect: (rowId) => void s.selectLayer(rowId),
          options: rectsOptionsFrom(o),
        }),
      ]),
    ]);
  }

  // ---- Hierarchy ----
  private buildTree(s: SurfaceFlingerSession): TreeItem[] {
    const o = s.options;
    let pool = s.layers;
    if (o.hierOnlyVisible) pool = pool.filter((l) => l.isVisible);
    if (o.hierSearch.trim()) {
      const q = o.hierSearch.toLowerCase();
      const keep = new Set<number>();
      for (const l of s.layers) {
        if (l.name.toLowerCase().includes(q) || String(l.layerId).includes(q)) {
          let cur: SfLayer | undefined = l;
          while (cur && !keep.has(cur.layerId)) {
            keep.add(cur.layerId);
            cur = cur.parent >= 0 ? s.byLayerId.get(cur.parent) : undefined;
          }
        }
      }
      pool = pool.filter((l) => keep.has(l.layerId));
    }
    const inPool = new Set(pool.map((l) => l.layerId));
    const sortFn = (a: SfLayer, b: SfLayer) =>
      (a.drawDepth ?? 1e9) - (b.drawDepth ?? 1e9);
    const ghosts: TreeItem[] = o.diff
      ? s.deletedLayers().map((layer) => ({layer, children: [], deleted: true}))
      : [];
    if (o.hierFlat) {
      return [
        ...[...pool].sort(sortFn).map((layer) => ({layer, children: []})),
        ...ghosts,
      ];
    }
    const childrenOf = new Map<number, SfLayer[]>();
    const roots: SfLayer[] = [];
    for (const l of pool) {
      if (l.parent >= 0 && inPool.has(l.parent)) {
        const arr = childrenOf.get(l.parent);
        if (arr) arr.push(l);
        else childrenOf.set(l.parent, [l]);
      } else {
        roots.push(l);
      }
    }
    const build = (l: SfLayer): TreeItem => ({
      layer: l,
      children: (childrenOf.get(l.layerId) ?? []).sort(sortFn).map(build),
    });
    return [...roots.sort(sortFn).map(build), ...ghosts];
  }

  private renderNode(
    s: SurfaceFlingerSession,
    item: TreeItem,
    dupIds: Set<number>,
  ): m.Children {
    const l = item.layer;
    const selected = l.rowId === s.selectedRowId;
    const status = item.deleted ? 'deleted' : s.diffStatusOf(l);
    const cls = [
      selected ? 'pf-sf-hnode--sel' : '',
      status !== 'unchanged' ? `pf-sf-hnode--${status}` : '',
    ]
      .filter(Boolean)
      .join(' ');
    const hidden = s.hiddenLayerIds.has(l.layerId);
    const pinned = s.pinnedLayerIds.has(l.layerId);
    const label = m(
      'span.pf-sf-hnode',
      {
        class: cls,
        onclick: (e: Event) => {
          e.stopPropagation();
          if (!item.deleted) void s.selectLayer(l.rowId);
        },
      },
      [
        m('span.pf-sf-hnode__id', `${l.layerId}`),
        m('span.pf-sf-hnode__name', simplify(l.name, s.options.hierSimplify)),
        ...chipsFor(l, dupIds.has(l.layerId), s.relZParentIds.has(l.layerId)),
        !item.deleted &&
          m(Button, {
            className: 'pf-sf-hnode__btn',
            compact: true,
            icon: hidden ? 'visibility_off' : 'visibility',
            title: hidden ? 'Show rect' : 'Hide rect',
            onclick: (e: Event) => {
              e.stopPropagation();
              s.toggleHidden(l.layerId);
            },
          }),
        !item.deleted &&
          m(Button, {
            className: 'pf-sf-hnode__btn',
            compact: true,
            active: pinned,
            icon: 'push_pin',
            title: pinned ? 'Unpin rect' : 'Pin rect',
            onclick: (e: Event) => {
              e.stopPropagation();
              s.togglePinned(l.layerId);
            },
          }),
      ],
    );
    return m(
      TreeNode,
      {left: label, startsCollapsed: false},
      item.children.map((c) => this.renderNode(s, c, dupIds)),
    );
  }

  private renderHierarchy(s: SurfaceFlingerSession): m.Children {
    const o = s.options;
    const counts = new Map<number, number>();
    for (const l of s.layers) {
      counts.set(l.layerId, (counts.get(l.layerId) ?? 0) + 1);
    }
    const dupIds = new Set(
      [...counts].filter(([, n]) => n > 1).map(([id]) => id),
    );
    return m('.pf-sf-pane.pf-sf-pane--hier', [
      m(Section, {title: 'Hierarchy'}, [
        m('.pf-sf-toolbar', [
          m(Checkbox, {
            label: 'Only visible',
            checked: o.hierOnlyVisible,
            onchange: () => (o.hierOnlyVisible = !o.hierOnlyVisible),
          }),
          m(Checkbox, {
            label: 'Flat',
            checked: o.hierFlat,
            onchange: () => (o.hierFlat = !o.hierFlat),
          }),
          m(Checkbox, {
            label: 'Simplify',
            checked: o.hierSimplify,
            onchange: () => (o.hierSimplify = !o.hierSimplify),
          }),
          m(Checkbox, {
            label: 'Diff',
            checked: o.diff,
            onchange: () => void s.setDiff(!o.diff),
          }),
        ]),
        m(TextInput, {
          placeholder: 'Filter layers…',
          value: o.hierSearch,
          oninput: (e: Event) =>
            (o.hierSearch = (e.target as HTMLInputElement).value),
        }),
        m(
          Tree,
          {className: 'pf-sf-htree'},
          this.buildTree(s).map((item) => this.renderNode(s, item, dupIds)),
        ),
      ]),
    ]);
  }

  // ---- Properties (curated + full proto) ----
  // The curated summary as a DataGrid: section titles become header rows, and
  // values that reference another layer (occluded-by, relative-Z parent) render
  // as clickable links via a cell renderer.
  private renderCurated(s: SurfaceFlingerSession): m.Children {
    const layer = s.selectedLayer;
    if (!layer) return m('.pf-sf-empty', 'Select a layer.');
    const c =
      this.curatedCache?.layer === layer &&
      this.curatedCache.args === s.selectedArgs
        ? this.curatedCache
        : (this.curatedCache = this.buildCurated(s, layer));
    if (c.count === 0) return m('.pf-sf-empty', 'No properties.');
    const h = Math.min(
      c.count * CURATED_ROW_PX + CURATED_PAD_PX,
      CURATED_MAX_PX,
    );
    return m(
      '.pf-sf-curated',
      {style: {height: `${h}px`}},
      m(DataGrid, {
        schema: c.schema,
        rootSchema: 'root',
        data: c.ds,
        columns: CURATED_COLUMNS,
        fillHeight: true,
        disableColumnControls: true,
        disableFilterControls: true,
        disablePivotControls: true,
      }),
    );
  }

  private buildCurated(
    s: SurfaceFlingerSession,
    layer: SfLayer,
  ): NonNullable<SfViewer['curatedCache']> {
    const sections = curatedProperties(layer, s.selectedArgs, (id) =>
      s.nameOf(id),
    );
    // The DataGrid's in-memory source projects rows to the declared columns, so
    // header-ness and layer references are derived from the two visible columns
    // (the section titles and the per-row label) rather than carried as hidden
    // fields.
    const headerTitles = new Set<string>();
    const refByLabel = new Map<string, number>();
    const rows: Array<Record<string, SqlValue>> = [];
    for (const sec of sections) {
      headerTitles.add(sec.title);
      rows.push({property: sec.title, value: ''});
      for (const r of sec.rows) {
        if (r.layerRefs && r.layerRefs.length > 0) {
          refByLabel.set(r.label, r.layerRefs[0]);
        }
        rows.push({property: r.label, value: r.value});
      }
    }
    const schema: SchemaRegistry = {
      root: {
        property: {
          title: 'Property',
          columnType: 'text',
          cellRenderer: (v: SqlValue) =>
            headerTitles.has(String(v))
              ? m('span.pf-sf-cur-h', String(v))
              : String(v ?? ''),
        },
        value: {
          title: 'Value',
          columnType: 'text',
          cellRenderer: (v: SqlValue, row: Record<string, SqlValue>) => {
            const ref = refByLabel.get(String(row.property));
            if (ref !== undefined) {
              return m(
                'a.pf-sf-ref',
                {onclick: () => s.selectLayerByLayerId(ref)},
                String(v ?? ''),
              );
            }
            return String(v ?? '');
          },
        },
      },
    };
    return {
      layer,
      args: s.selectedArgs,
      ds: new InMemoryDataSource(rows),
      schema,
      count: rows.length,
    };
  }

  private renderPropHeader(
    s: SurfaceFlingerSession,
    layer: SfLayer,
  ): m.Children {
    const dup = s.layers.filter((l) => l.layerId === layer.layerId).length > 1;
    return m('.pf-sf-prophead', [
      m('.pf-sf-prophead__name', `${layer.name}  #${layer.layerId}`),
      m(
        '.pf-sf-prophead__chips',
        chipsFor(layer, dup, s.relZParentIds.has(layer.layerId)),
      ),
    ]);
  }

  private renderProtoGrid(s: SurfaceFlingerSession): m.Children {
    const diff = s.options.diff;
    const showDefaults = s.options.propShowDefaults;
    const c = this.protoCache;
    const cached =
      c !== undefined &&
      c.args === s.selectedArgs &&
      c.showDefaults === showDefaults &&
      c.diff === diff &&
      c.prev === s.prevArgByKey
        ? c
        : (this.protoCache = this.buildProto(s, diff, showDefaults));
    if (cached.count === 0) return m('.pf-sf-empty', 'No properties.');
    return m(
      '.pf-sf-proto',
      m(DataGrid, {
        schema: diff ? PROTO_SCHEMA_DIFF : PROTO_SCHEMA,
        rootSchema: 'root',
        data: cached.ds,
        columns: diff ? PROTO_COLUMNS_DIFF : PROTO_COLUMNS,
        fillHeight: true,
      }),
    );
  }

  private buildProto(
    s: SurfaceFlingerSession,
    diff: boolean,
    showDefaults: boolean,
  ): NonNullable<SfViewer['protoCache']> {
    const rows = s.selectedArgs
      .filter((a) => showDefaults || !isDefaultish(a))
      .map((a) => {
        const value = String(a.value);
        const row: Record<string, string> = {
          property: a.key,
          value,
          type: a.type,
        };
        if (diff) {
          const p = s.prevArgByKey.get(a.key);
          row.previous = p !== undefined && p !== value ? p : '';
        }
        return row;
      });
    return {
      args: s.selectedArgs,
      showDefaults,
      diff,
      prev: s.prevArgByKey,
      ds: new InMemoryDataSource(rows),
      count: rows.length,
    };
  }

  private renderProperties(s: SurfaceFlingerSession): m.Children {
    const layer = s.selectedLayer;
    return m('.pf-sf-pane.pf-sf-pane--props', [
      layer ? this.renderPropHeader(s, layer) : null,
      m(Section, {title: 'Properties'}, this.renderCurated(s)),
      m(Section, {title: 'Proto dump'}, [
        m('.pf-sf-toolbar', [
          m(Checkbox, {
            label: 'Show defaults',
            checked: s.options.propShowDefaults,
            onchange: () =>
              (s.options.propShowDefaults = !s.options.propShowDefaults),
          }),
        ]),
        this.renderProtoGrid(s),
      ]),
    ]);
  }
}
