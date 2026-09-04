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

// Shared state + data loading for the SurfaceFlinger viewer, owned by the
// plugin and consumed by both the full-screen page and the timeline track's
// "open in viewer" deep link. Holds the selected display, the current snapshot,
// the loaded layers, the selected layer (+ its proto args), and the view
// options so they persist as you scrub.

import m from 'mithril';
import type {Trace} from '../../public/trace';
import {
  queryArgs,
  queryDisplays,
  queryLayers,
  querySnapshots,
  type SfArg,
  type SfDisplay,
  type SfLayer,
  type SfSnapshot,
} from './surfaceflinger_data';

export type ShadingMode = 'gradient' | 'opacity' | 'wireframe';

export interface SfViewOptions {
  rectsOnlyVisible: boolean;
  explode: number;
  rotation: number; // 0..1 yaw/pitch of the 3D rects view
  shading: ShadingMode;
  hierOnlyVisible: boolean;
  hierFlat: boolean;
  hierSimplify: boolean;
  hierSearch: string;
  propShowDefaults: boolean;
  diff: boolean; // diff this snapshot against the previous one
}

export type DiffStatus = 'added' | 'deleted' | 'modified' | 'unchanged';

export class SurfaceFlingerSession {
  readonly trace: Trace;
  displays: SfDisplay[] = [];
  displayId?: string;
  snapshots: SfSnapshot[] = [];
  index = 0;

  layers: SfLayer[] = [];
  byRowId = new Map<number, SfLayer>();
  byLayerId = new Map<number, SfLayer>();
  selectedRowId?: number;
  selectedArgs: SfArg[] = [];
  // layerId of the current selection, so it persists across snapshots.
  private keepLayerId?: number;

  // Previous snapshot (for diff) and per-layerId signatures of it.
  prevLayers: SfLayer[] = [];
  prevByLayerId = new Map<number, SfLayer>();
  // base64_proto_id of each previous-snapshot layer, for the exact diff.
  private prevProtoByLayerId = new Map<number, number | undefined>();
  // Property values of the selected layer in the previous snapshot, by arg key.
  prevArgByKey = new Map<string, string>();

  // Per-layerId so they persist as you scrub.
  readonly hiddenLayerIds = new Set<number>();
  readonly pinnedLayerIds = new Set<number>();
  // Layers that other layers are Z-ordered relative to (for the RelZParent chip).
  relZParentIds = new Set<number>();

  loadingLayers = false;
  // Monotonic tokens to discard superseded async work: loadToken guards a
  // display/snapshot/layer load; argToken guards a selection's arg fetch. They
  // are independent so selecting a layer never cancels an in-flight layer load.
  private loadToken = 0;
  private argToken = 0;

  readonly options: SfViewOptions = {
    rectsOnlyVisible: true,
    explode: 0,
    rotation: 0,
    shading: 'gradient',
    hierOnlyVisible: false,
    hierFlat: false,
    hierSimplify: true,
    hierSearch: '',
    propShowDefaults: false,
    diff: false,
  };

  constructor(trace: Trace) {
    this.trace = trace;
  }

  get displayName(): string {
    return (
      this.displays.find((d) => d.displayId === this.displayId)?.displayName ??
      'Display'
    );
  }

  // Layer-stack/group of the selected display.
  get selectedDisplayGroup(): number | undefined {
    return this.displays.find((d) => d.displayId === this.displayId)?.group;
  }

  // Layers composited on the selected display (by group). The surface view uses
  // this so each display shows only its own composition; the hierarchy and
  // properties use the full layer set (one cross-display tree).
  displayLayers(): SfLayer[] {
    const g = this.selectedDisplayGroup;
    if (g === undefined) return this.layers;
    return this.layers.filter((l) => l.groupId === g);
  }

  get currentSnapshot(): SfSnapshot | undefined {
    return this.snapshots[this.index];
  }

  async init(): Promise<void> {
    this.displays = await queryDisplays(this.trace.engine);
    if (this.displays.length > 0) {
      // Default to a real display, not a transient encoder/mirror (virtual) one.
      const def = this.displays.find((d) => !d.isVirtual) ?? this.displays[0];
      await this.setDisplay(def.displayId);
    }
  }

  async setDisplay(displayId: string): Promise<void> {
    const token = ++this.loadToken;
    this.displayId = displayId;
    const snapshots = await querySnapshots(this.trace.engine, displayId);
    if (token !== this.loadToken) return; // superseded
    this.snapshots = snapshots;
    await this.loadIndex(0, token);
  }

  async setIndex(i: number): Promise<void> {
    await this.loadIndex(i, ++this.loadToken);
  }

  // Jump to the snapshot whose timestamp is nearest ts (used by the timeline
  // track's "open in viewer" deep link).
  async setNearestTs(ts: bigint): Promise<void> {
    if (this.snapshots.length === 0) return;
    let best = 0;
    let bestD = -1n;
    for (let i = 0; i < this.snapshots.length; i++) {
      const d =
        this.snapshots[i].ts > ts
          ? this.snapshots[i].ts - ts
          : ts - this.snapshots[i].ts;
      if (bestD < 0n || d < bestD) {
        bestD = d;
        best = i;
      }
    }
    await this.loadIndex(best, ++this.loadToken);
  }

  // Loads the layers for snapshot `i`. `token` is the caller's loadToken; all
  // state writes are gated on it still being current so a superseded load (a
  // newer scrub / display change) never clobbers the winning one.
  private async loadIndex(i: number, token: number): Promise<void> {
    if (token !== this.loadToken) return;
    if (this.snapshots.length === 0) {
      this.index = 0;
      this.layers = [];
      this.loadingLayers = false;
      m.redraw();
      return;
    }
    this.index = Math.max(0, Math.min(i, this.snapshots.length - 1));
    const snap = this.snapshots[this.index];
    this.loadingLayers = true;
    try {
      const layers = await queryLayers(this.trace.engine, snap.snapshotId);
      if (token !== this.loadToken) return;
      this.layers = layers;
      this.byRowId = new Map(layers.map((l) => [l.rowId, l]));
      this.byLayerId = new Map(layers.map((l) => [l.layerId, l]));
      this.relZParentIds = new Set(
        layers
          .filter((l) => l.zOrderRelativeOf > 0)
          .map((l) => l.zOrderRelativeOf),
      );
      await this.loadPrev();
      if (token !== this.loadToken) return;
      // Keep the same layer (by layerId) selected across snapshots if present,
      // else default to the front-most visible layer with a rect.
      let def: SfLayer | undefined;
      for (const l of layers) {
        if (this.keepLayerId !== undefined && l.layerId === this.keepLayerId) {
          def = l;
          break;
        }
        if (
          l.isVisible &&
          l.rect &&
          (def === undefined || (l.drawDepth ?? 0) > (def.drawDepth ?? 0))
        ) {
          def = l;
        }
      }
      def ??= layers.at(0);
      this.selectedRowId = undefined;
      this.selectedArgs = [];
      if (def !== undefined) await this.selectLayer(def.rowId);
    } finally {
      if (token === this.loadToken) this.loadingLayers = false;
    }
    m.redraw();
  }

  async selectLayer(rowId: number): Promise<void> {
    const token = ++this.argToken;
    this.selectedRowId = rowId;
    const layer = this.byRowId.get(rowId);
    this.keepLayerId = layer?.layerId;
    const args =
      layer?.argSetId !== undefined
        ? await queryArgs(this.trace.engine, layer.argSetId)
        : [];
    if (token !== this.argToken) return; // superseded by a newer selection
    this.selectedArgs = args;
    // Previous-snapshot values of the same layer, for the property diff.
    const prevArgByKey = new Map<string, string>();
    if (this.options.diff && layer) {
      const pl = this.prevByLayerId.get(layer.layerId);
      if (pl?.argSetId !== undefined) {
        const pa = await queryArgs(this.trace.engine, pl.argSetId);
        if (token !== this.argToken) return;
        for (const a of pa) prevArgByKey.set(a.key, String(a.value));
      }
    }
    this.prevArgByKey = prevArgByKey;
    m.redraw();
  }

  selectLayerByLayerId(layerId: number): void {
    const t = this.byLayerId.get(layerId);
    if (t) void this.selectLayer(t.rowId);
  }

  nameOf(layerId: number): string {
    return this.byLayerId.get(layerId)?.name ?? `Layer ${layerId}`;
  }

  get selectedLayer(): SfLayer | undefined {
    return this.selectedRowId !== undefined
      ? this.byRowId.get(this.selectedRowId)
      : undefined;
  }

  // ---- Diff ----
  private async loadPrev(): Promise<void> {
    if (!this.options.diff || this.index <= 0) {
      this.prevLayers = [];
      this.prevByLayerId = new Map();
      this.prevProtoByLayerId = new Map();
      return;
    }
    const prev = await queryLayers(
      this.trace.engine,
      this.snapshots[this.index - 1].snapshotId,
    );
    this.prevLayers = prev;
    this.prevByLayerId = new Map(prev.map((l) => [l.layerId, l]));
    this.prevProtoByLayerId = new Map(
      prev.map((l) => [l.layerId, l.base64ProtoId]),
    );
  }

  // Exact: identical LayerProtos share one base64_proto_id, so an id change
  // means some property changed (an any-property diff).
  diffStatusOf(l: SfLayer): DiffStatus {
    if (!this.options.diff || this.index <= 0) return 'unchanged';
    if (!this.prevProtoByLayerId.has(l.layerId)) return 'added';
    const prevId = this.prevProtoByLayerId.get(l.layerId);
    return prevId === l.base64ProtoId ? 'unchanged' : 'modified';
  }

  // Layers present in the previous snapshot but gone in this one.
  deletedLayers(): SfLayer[] {
    if (!this.options.diff || this.index <= 0) return [];
    const cur = new Set(this.layers.map((l) => l.layerId));
    return this.prevLayers.filter((l) => !cur.has(l.layerId));
  }

  async setDiff(on: boolean): Promise<void> {
    this.options.diff = on;
    await this.loadPrev();
    if (this.selectedRowId !== undefined) {
      await this.selectLayer(this.selectedRowId);
    }
    m.redraw();
  }

  // ---- Per-rect hide / pin (by layerId, persist across snapshots) ----
  toggleHidden(layerId: number): void {
    if (this.hiddenLayerIds.has(layerId)) this.hiddenLayerIds.delete(layerId);
    else this.hiddenLayerIds.add(layerId);
    m.redraw();
  }
  togglePinned(layerId: number): void {
    if (this.pinnedLayerIds.has(layerId)) this.pinnedLayerIds.delete(layerId);
    else this.pinnedLayerIds.add(layerId);
    m.redraw();
  }
}
