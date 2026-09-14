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

import {HighPrecisionTimeSpan} from '../../base/high_precision_time_span';
import {type time, Time} from '../../base/time';
import type {YMode} from '../../components/tracks/counter_track';
import type {Trace} from '../../public/trace';
import {
  CounterSeries,
  type CounterTrackInfo,
  type LineStyle,
  type NameBy,
  defaultYMode,
  listCounterTracks,
} from './counter_index';

export type YScale = 'linear' | 'log';
export type YRangeMode = 'fit' | 'global' | 'shared';
export type LayoutMode = 'stacked' | 'overlay';

// Never let the visible window collapse below this (1us) — keeps zoom sane.
const MIN_VIEW_DUR = 1_000n;

// Holds all mutable state for one instance of the timeseries viewer: the
// available counters, which lanes are shown (and in what order), the shared
// visible time window, and the y-axis scale. Zoom/pan helpers clamp to the
// trace bounds so the view can never wander off the data.
export class ViewerModel {
  readonly trace: Trace;
  readonly traceStart: time;
  readonly traceEnd: time;

  all: ReadonlyArray<CounterTrackInfo> = [];
  selected: CounterSeries[] = [];
  // Ids currently selected, kept in sync with `selected` for O(1) membership
  // checks (the picker calls isSelected() per row, so this must not be O(n)).
  private readonly selectedIds = new Set<number>();
  // The plotted set is the union of counters added via the picker (manualIds)
  // and those from the timeline's area selection (areaIds). A new area selection
  // resets both to just that selection (see setAreaSelection); only the picker's
  // "+" accumulates.
  private readonly manualIds = new Set<number>();
  private readonly areaIds = new Set<number>();
  yScale: YScale = 'linear';
  yMode: YMode = 'value';
  // Y-axis range source (default 'global'). 'global' locks each axis to the
  // counter's whole-trace range, so magnitudes are comparable across zoom levels
  // and the axis never moves; 'shared' puts *every* visible lane/line on one
  // common range (the combined whole-trace range) so their heights are directly
  // comparable to each other; 'fit' fits each axis to the samples currently in
  // view (rescales continuously as you zoom).
  yRangeMode: YRangeMode = 'global';
  // Default line-naming scheme (per-line overrides win). Metric by default.
  nameBy: NameBy = 'metric';
  layout: LayoutMode = 'stacked';
  // Fill the area under each line in stacked mode (lighter than the line).
  fill = true;
  // Overlay-mode axis: true = each line normalised to its own range on a
  // 0-100% axis (good for comparing shapes); false = one shared absolute axis
  // labelled with the counters' unit (good for comparing magnitudes). Units by
  // default.
  overlayNormalize = false;
  // Overlay-mode stacked-area chart: areas are stacked cumulatively on a shared
  // absolute axis (implies a value axis, not %).
  overlayStacked = false;
  // Overlay value/stacked axis unit filter: when the selection mixes units, this
  // scopes the shared axis (and which lines are drawn) to one unit. undefined =
  // all units on one axis.
  overlayUnitFilter?: string;
  // Per-lane height in px (stacked mode). Smaller default than a fit-to-height
  // layout so lanes aren't huge; adjustable via the toolbar or by dragging a
  // lane's bottom edge. When the stack is taller than the viewport it scrolls.
  laneHeight = 96;
  // Overlay plot height in px; undefined = fill the viewport. Drag the plot's
  // bottom edge to resize.
  overlayHeight?: number;
  // Legend strip max-height in px (drag the chart's top edge to enlarge it and
  // see more chips); undefined = the compact default.
  legendHeight?: number;
  loading = true;

  // Bumped whenever the selection set changes, so the chart knows to refetch.
  selectionGeneration = 0;
  // Listeners woken after any state change so the (idle-stopped) render loop
  // redraws. A set (not a single callback) so more than one chart can share a
  // model — the contextual area-selection tab and the command-opened drawer tab
  // render the same model, and Gate keeps both mounted.
  private readonly invalidateListeners = new Set<() => void>();

  // Registers a redraw listener; returns a disposer to remove it.
  addInvalidateListener(fn: () => void): () => void {
    this.invalidateListeners.add(fn);
    return () => this.invalidateListeners.delete(fn);
  }

  private invalidate(): void {
    for (const fn of this.invalidateListeners) fn();
  }

  constructor(trace: Trace) {
    this.trace = trace;
    this.traceStart = trace.traceInfo.start;
    this.traceEnd = trace.traceInfo.end;
  }

  async init(): Promise<void> {
    this.all = await listCounterTracks(this.trace.engine);
    this.loading = false;
    this.invalidate();
  }

  // Rebuilds `selected` to exactly match manualIds ∪ areaIds, preserving the
  // order of already-selected series and disposing removed ones. All selection
  // mutations go through here so the two sources stay consistent.
  private reconcile(): void {
    const want = new Set<number>([...this.areaIds, ...this.manualIds]);
    const byId = new Map(this.all.map((i) => [i.id, i]));
    let changed = false;
    const removed: CounterSeries[] = [];
    this.selected = this.selected.filter((s) => {
      if (want.has(s.info.id)) return true;
      this.selectedIds.delete(s.info.id);
      removed.push(s);
      changed = true;
      return false;
    });
    for (const id of want) {
      if (this.selectedIds.has(id)) continue;
      const info = byId.get(id);
      if (info === undefined) continue;
      this.selected.push(this.makeSeries(info));
      this.selectedIds.add(id);
      changed = true;
    }
    // Only fetched series hold a mipmap; disposing unfetched ones is a no-op.
    if (removed.length > 0) void Promise.all(removed.map((s) => s.dispose()));
    if (changed) {
      this.selectionGeneration++;
      this.invalidate();
    }
  }

  // Sets the counters coming from the timeline's area selection. A *changed*
  // selection resets the plot to exactly those counters — any manual picker adds
  // from before are dropped, so re-selecting different tracks starts fresh. Only
  // the drawer's "+" button accumulates onto the current set. An unchanged
  // selection is a no-op, so manual adds survive while the selection is stable.
  setAreaSelection(ids: ReadonlyArray<number>): void {
    const next = new Set(ids);
    if (
      next.size === this.areaIds.size &&
      [...next].every((id) => this.areaIds.has(id))
    ) {
      return; // unchanged
    }
    this.areaIds.clear();
    for (const id of ids) this.areaIds.add(id);
    this.manualIds.clear();
    this.reconcile();
  }

  setNameBy(nameBy: NameBy): void {
    if (nameBy === this.nameBy) return;
    this.nameBy = nameBy;
    this.invalidate();
  }

  // Per-line display-name override (empty string clears it).
  setName(series: CounterSeries, name: string): void {
    const trimmed = name.trim();
    series.nameOverride = trimmed.length > 0 ? trimmed : undefined;
    this.invalidate();
  }

  setYScale(scale: YScale): void {
    if (scale === this.yScale) return;
    this.yScale = scale;
    this.invalidate();
  }

  setYRangeMode(mode: YRangeMode): void {
    if (mode === this.yRangeMode) return;
    this.yRangeMode = mode;
    this.invalidate();
  }

  setLayout(layout: LayoutMode): void {
    if (layout === this.layout) return;
    this.layout = layout;
    this.invalidate();
  }

  setLaneHeight(px: number): void {
    if (px === this.laneHeight) return;
    this.laneHeight = px;
    // The global control resets any per-lane overrides so it stays authoritative.
    for (const s of this.selected) s.laneHeightOverride = undefined;
    this.selectionGeneration++;
    this.invalidate();
  }

  // Per-lane height override (stacked mode; drag a lane's bottom edge).
  // Bumps the generation so the chart re-fetches: a resize can push a
  // previously off-screen lane into view.
  setSeriesLaneHeight(series: CounterSeries, px: number): void {
    if (series.laneHeightOverride === px) return;
    series.laneHeightOverride = px;
    this.selectionGeneration++;
    this.invalidate();
  }

  setFill(fill: boolean): void {
    if (fill === this.fill) return;
    this.fill = fill;
    this.invalidate();
  }

  setOverlayNormalize(normalize: boolean): void {
    if (normalize === this.overlayNormalize) return;
    this.overlayNormalize = normalize;
    this.invalidate();
  }

  setOverlayStacked(stacked: boolean): void {
    if (stacked === this.overlayStacked) return;
    this.overlayStacked = stacked;
    // Stacking is a shared absolute axis; % normalisation is meaningless there.
    if (stacked) this.overlayNormalize = false;
    this.invalidate();
  }

  setOverlayUnitFilter(unit: string | undefined): void {
    if (unit === this.overlayUnitFilter) return;
    this.overlayUnitFilter = unit;
    this.invalidate();
  }

  // Distinct units among the selected counters (for the overlay unit picker).
  selectedUnits(): string[] {
    const set = new Set<string>();
    for (const s of this.selected) {
      if (s.info.unit !== undefined && s.info.unit.length > 0) {
        set.add(s.info.unit);
      }
    }
    return [...set].sort();
  }

  setOverlayHeight(px: number | undefined): void {
    this.overlayHeight = px;
    this.invalidate();
  }

  setLegendHeight(px: number): void {
    this.legendHeight = px;
    this.invalidate();
  }

  // Moves a lane to a new position among the *visible* lanes (drag-to-reorder in
  // stacked mode). Hidden lanes keep their relative order.
  reorder(series: CounterSeries, toVisibleIdx: number): void {
    const visible = this.selected.filter((s) => !s.hidden);
    const from = visible.indexOf(series);
    if (from < 0) return;
    const to = Math.max(0, Math.min(toVisibleIdx, visible.length - 1));
    if (to === from) return;
    const anchor = visible[to]; // the visible lane we drop onto
    const arr = this.selected.filter((s) => s !== series);
    const anchorIdx = arr.indexOf(anchor);
    arr.splice(to > from ? anchorIdx + 1 : anchorIdx, 0, series);
    this.selected = arr;
    this.selectionGeneration++;
    this.invalidate();
  }

  // Per-line value/delta/rate (rebuilds just this series' mipmap).
  async setSeriesYMode(series: CounterSeries, mode: YMode): Promise<void> {
    await series.setYMode(mode);
    this.selectionGeneration++;
    this.invalidate();
  }

  // Maps a counter_track id to the uri of the timeline track that renders it,
  // so we can read that track's user-facing settings. Built once (lazily).
  private uriByCounterId?: Map<number, string>;
  private counterUri(id: number): string | undefined {
    if (this.uriByCounterId === undefined) {
      this.uriByCounterId = new Map();
      for (const node of this.trace.workspaces.currentWorkspace.flatTracks) {
        const uri = node.uri;
        if (uri === undefined) continue;
        const ids = this.trace.tracks.getTrack(uri)?.tags?.trackIds;
        if (ids) {
          for (const cid of ids) {
            if (!this.uriByCounterId.has(cid)) {
              this.uriByCounterId.set(cid, uri);
            }
          }
        }
      }
    }
    return this.uriByCounterId.get(id);
  }

  // The y-mode the user has set on this counter's timeline track (its "Y Mode"
  // setting), so the viewer mirrors exactly what they see in the timeline.
  private trackYMode(id: number): YMode | undefined {
    const uri = this.counterUri(id);
    if (uri === undefined) return undefined;
    const s = this.trace.tracks
      .getTrack(uri)
      ?.renderer?.settings?.find((x) => x.descriptor.name === 'Y Mode');
    const v = s?.value;
    return v === 'value' || v === 'delta' || v === 'rate' ? v : undefined;
  }

  private makeSeries(info: CounterTrackInfo): CounterSeries {
    // Prefer the mode the user set on the counter's timeline track so the
    // viewer maps exactly; otherwise fall back to a sensible default
    // (cumulative -> rate). The global Value/Delta/Rate toolbar overrides all.
    const mode = this.trackYMode(info.id) ?? defaultYMode(info);
    return new CounterSeries(this.trace.engine, info, this.traceStart, mode);
  }

  // Switches value/delta/rate for every lane; rebuilds mipmaps lazily.
  async setYMode(mode: YMode): Promise<void> {
    if (mode === this.yMode) return;
    this.yMode = mode;
    await Promise.all(this.selected.map((s) => s.setYMode(mode)));
    this.selectionGeneration++;
    this.invalidate();
  }

  toggleHidden(series: CounterSeries): void {
    series.hidden = !series.hidden;
    this.selectionGeneration++;
    this.invalidate();
  }

  // Stacked-mode multi-select: click lanes to emphasise them. While any lane is
  // emphasised the rest are greyed and sink to the bottom; clicking an
  // emphasised lane again de-selects it, and clicking empty space clears all.
  toggleEmphasis(series: CounterSeries): void {
    series.emphasized = !series.emphasized;
    this.selectionGeneration++; // changes lane order / the on-screen set
    this.invalidate();
  }

  hasEmphasis(): boolean {
    return this.selected.some((s) => s.emphasized && !s.hidden);
  }

  clearEmphasis(): void {
    let changed = false;
    for (const s of this.selected) {
      if (s.emphasized) {
        s.emphasized = false;
        changed = true;
      }
    }
    if (changed) {
      this.selectionGeneration++;
      this.invalidate();
    }
  }

  // True when exactly one of several lines is showing — i.e. the view is
  // isolated to a single line. Used so a click on empty space un-isolates.
  isIsolated(): boolean {
    if (this.selected.length < 2) return false;
    return this.selected.filter((s) => !s.hidden).length === 1;
  }

  // Isolate one line: show only it, hide all others (click-to-focus). If it is
  // already the only visible line, show everything again (click to un-isolate).
  toggleOnly(series: CounterSeries): void {
    const isolated =
      !series.hidden && this.selected.every((s) => s === series || s.hidden);
    if (isolated) {
      this.setAllHidden(false);
    } else {
      this.showOnly(series);
    }
  }

  private showOnly(series: CounterSeries): void {
    let changed = false;
    for (const s of this.selected) {
      const hide = s !== series;
      if (s.hidden !== hide) {
        s.hidden = hide;
        changed = true;
      }
    }
    if (changed) {
      this.selectionGeneration++;
      this.invalidate();
    }
  }

  // Show or hide every selected line at once (legend bulk action): hide all,
  // then click individual chips to bring them back one at a time.
  setAllHidden(hidden: boolean): void {
    let changed = false;
    for (const s of this.selected) {
      if (s.hidden !== hidden) {
        s.hidden = hidden;
        changed = true;
      }
    }
    if (changed) {
      this.selectionGeneration++;
      this.invalidate();
    }
  }

  setLineStyle(series: CounterSeries, style: LineStyle): void {
    series.lineStyle = style;
    this.selectionGeneration++;
    this.invalidate();
  }

  // Sets a per-line colour override (undefined restores the palette default).
  setColor(series: CounterSeries, color: string | undefined): void {
    series.colorOverride = color;
    this.selectionGeneration++;
    this.invalidate();
  }

  // The visible window is the main timeline's, so the chart pans/zooms in
  // lock-step with the tracks above.
  get viewStart(): time {
    return this.trace.timeline.visibleWindow.start.toTime('floor');
  }
  get viewEnd(): time {
    return this.trace.timeline.visibleWindow.end.toTime('ceil');
  }
  get viewDuration(): bigint {
    return this.viewEnd - this.viewStart;
  }

  isSelected(id: number): boolean {
    return this.selectedIds.has(id);
  }

  // Picker toggle: add to / remove from the manual set. Removing also drops it
  // from the area set, so a manually-removed line stays gone until the timeline
  // selection changes again.
  toggleTrack(info: CounterTrackInfo): void {
    if (this.selectedIds.has(info.id)) {
      this.manualIds.delete(info.id);
      this.areaIds.delete(info.id);
    } else {
      this.manualIds.add(info.id);
    }
    this.reconcile();
  }

  // Adds many counters via the picker (manual set). Cheap even for very large
  // traces: CounterSeries are lightweight and build no mipmap until fetched,
  // and only the lanes actually on screen are ever fetched/drawn.
  selectMany(infos: ReadonlyArray<CounterTrackInfo>): void {
    for (const info of infos) this.manualIds.add(info.id);
    this.reconcile();
  }

  selectAll(): void {
    this.selectMany(this.all);
  }

  deselectMany(ids: ReadonlyArray<number>): void {
    for (const id of ids) {
      this.manualIds.delete(id);
      this.areaIds.delete(id);
    }
    this.reconcile();
  }

  deselectAll(): void {
    this.manualIds.clear();
    this.areaIds.clear();
    this.reconcile();
  }

  // Clamp + apply a new visible window, enforcing bounds and a min duration.
  setView(start: time, end: time): void {
    let s = start;
    let e = end;
    if (e - s < MIN_VIEW_DUR) {
      const mid = s + (e - s) / 2n;
      s = Time.fromRaw(mid - MIN_VIEW_DUR / 2n);
      e = Time.fromRaw(mid + MIN_VIEW_DUR / 2n);
    }
    // Shift back inside [traceStart, traceEnd] without changing duration where
    // possible, then clamp the ends.
    const dur = e - s;
    const total = this.traceEnd - this.traceStart;
    if (dur >= total) {
      s = this.traceStart;
      e = this.traceEnd;
    } else {
      if (s < this.traceStart) {
        s = this.traceStart;
        e = Time.fromRaw(s + dur);
      }
      if (e > this.traceEnd) {
        e = this.traceEnd;
        s = Time.fromRaw(e - dur);
      }
    }
    // The timeline owns the window; write it back so the whole timeline (and
    // any other synced views) pan/zoom together.
    this.trace.timeline.setVisibleWindow(
      HighPrecisionTimeSpan.fromTime(Time.fromRaw(s), Time.fromRaw(e)),
    );
    this.invalidate();
  }

  // Zoom by `factor` (>1 zooms out, <1 zooms in) keeping `centerTime` fixed.
  // The math is done relative to the window start so it stays exact for the
  // very large (boot-relative, weeks-scale) timestamps this plugin targets,
  // where absolute ns exceeds 2^53 and Number() would lose precision.
  zoomAt(centerTime: time, factor: number): void {
    const start = this.viewStart;
    const c = Number(centerTime - start); // ns from window start, safely < 2^53
    const e = Number(this.viewEnd - start);
    const ns = c - c * factor;
    const ne = c + (e - c) * factor;
    this.setView(
      Time.fromRaw(start + BigInt(Math.round(ns))),
      Time.fromRaw(start + BigInt(Math.round(ne))),
    );
  }

  panBy(deltaNs: number): void {
    const d = BigInt(Math.round(deltaNs));
    this.setView(
      Time.fromRaw(this.viewStart + d),
      Time.fromRaw(this.viewEnd + d),
    );
  }

  resetView(): void {
    this.setView(this.traceStart, this.traceEnd);
  }

  async dispose(): Promise<void> {
    const series = this.selected;
    this.selected = [];
    this.selectedIds.clear();
    this.manualIds.clear();
    this.areaIds.clear();
    await Promise.all(series.map((s) => s.dispose()));
  }
}
