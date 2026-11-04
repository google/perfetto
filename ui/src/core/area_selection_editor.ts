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

import {HighPrecisionTime} from '../base/high_precision_time';
import {HighPrecisionTimeSpan} from '../base/high_precision_time_span';
import {debounce} from '../base/rate_limiters';
import {Time, type time, type TimeSpan} from '../base/time';
import type {Area, Selection, SelectionOpts} from '../public/selection';
import {raf} from './raf_scheduler';

export interface EditableTimeline {
  readonly visibleWindow: HighPrecisionTimeSpan;
  setVisibleWindow(window: HighPrecisionTimeSpan): void;
}

export interface EditableSelectionManager {
  readonly selection: Selection;
  selectArea(area: Area, opts?: SelectionOpts): void;
}

const MIN_SELECTION_DURATION = 1;
const DETAILS_REDRAW_DELAY_MS = 100;

// Selection bounds as fractions of the visible window (0 = left edge).
interface ViewportAnchor {
  readonly start: number;
  readonly end: number;
}

/**
 * Edit mode for the area selection. While editing, the selection is anchored
 * to its position on screen: panning moves the timeline under the selection,
 * which is re-selected to cover whatever is beneath it. When the timeline is
 * clamped at the trace edge, the selection moves across the screen instead,
 * up to the trace edge. Zooming (see zoom()) resizes the selection rather
 * than the timeline, and step() moves it by exactly its own duration.
 *
 * A new area selection stays in edit mode; any other selection exits it.
 */
export class AreaSelectionEditor {
  private anchor?: ViewportAnchor;
  // The bounds step() moves from. Unlike the selection, these aren't clipped
  // to the trace, so stepping back from a partial tile at a trace edge
  // restores the full tile.
  private tile?: {readonly start: time; readonly end: time};
  private isUpdatingSelection = false;
  private isStepping = false;
  private readonly scheduleDetailsRedraw = debounce(
    () => raf.scheduleFullRedraw(),
    DETAILS_REDRAW_DELAY_MS,
  );

  constructor(
    private readonly timeline: EditableTimeline,
    private readonly selectionManager: EditableSelectionManager,
    private readonly traceSpan: TimeSpan,
  ) {}

  get isEditing(): boolean {
    return this.anchor !== undefined;
  }

  startEditing(): void {
    const selection = this.selectionManager.selection;
    if (selection.kind !== 'area') return;
    this.setAnchor(selection);
  }

  stopEditing(): void {
    this.anchor = undefined;
    this.tile = undefined;
  }

  toggleEditing(): void {
    if (this.isEditing) {
      this.stopEditing();
    } else {
      this.startEditing();
    }
  }

  onSelectionChanged(selection: Selection): void {
    if (this.isUpdatingSelection) return;
    if (this.anchor === undefined) return;

    if (selection.kind === 'area') {
      this.setAnchor(selection);
    } else {
      this.stopEditing();
    }
  }

  onVisibleWindowChanged(requested: HighPrecisionTimeSpan): void {
    if (this.anchor === undefined || this.isStepping) return;
    // Use the requested (unclamped) window so that any movement the timeline
    // couldn't make moves the selection instead.
    this.updateSelection(this.spanFor(this.anchor, requested));
  }

  zoom(ratio: number): void {
    if (this.anchor === undefined) return;
    const span = this.spanFor(this.anchor, this.timeline.visibleWindow);
    this.updateSelection(span.scale(ratio, 0.5, MIN_SELECTION_DURATION));
  }

  // Steps the selection by its own duration, keeping it in place on screen.
  step(direction: 1 | -1): void {
    const tile = this.tile;
    if (this.anchor === undefined || tile === undefined) return;
    const duration = tile.end - tile.start;
    if (duration <= 0n) return;
    const delta = BigInt(direction) * duration;
    const start = Time.fromRaw(tile.start + delta);
    const end = Time.fromRaw(tile.end + delta);
    const {start: traceStart, end: traceEnd} = this.traceSpan;
    if (start >= traceEnd || end <= traceStart) return;

    const selection = this.selectionManager.selection;
    if (selection.kind !== 'area') {
      this.stopEditing();
      return;
    }
    this.tile = {start, end};
    const area = {
      start: Time.max(start, traceStart),
      end: Time.min(end, traceEnd),
      trackUris: selection.trackUris,
    };
    this.select(area.start, area.end, area.trackUris);

    this.isStepping = true;
    try {
      const window = this.timeline.visibleWindow;
      this.timeline.setVisibleWindow(window.translate(Number(delta)));
    } finally {
      this.isStepping = false;
    }
    this.anchor = this.anchorForArea(area);
  }

  moveTo(start: time): void {
    const selection = this.selectionManager.selection;
    if (selection.kind !== 'area') return;
    const duration = selection.end - selection.start;
    const {start: traceStart, end: traceEnd} = this.traceSpan;
    const newStart = Time.max(
      traceStart,
      Time.min(start, Time.fromRaw(traceEnd - duration)),
    );
    if (newStart === selection.start) return;
    const area = {
      start: newStart,
      end: Time.fromRaw(newStart + duration),
      trackUris: selection.trackUris,
    };
    this.select(area.start, area.end, area.trackUris);
    if (this.anchor !== undefined) this.setAnchor(area);
  }

  private updateSelection(requested: HighPrecisionTimeSpan): void {
    const selection = this.selectionManager.selection;
    if (selection.kind !== 'area') {
      this.stopEditing();
      return;
    }

    const span = requested.fitWithin(this.traceSpan.start, this.traceSpan.end);
    this.anchor = this.anchorFor(span.start, span.end);

    const newStart = span.start.toTime('round');
    const newEnd = span.end.toTime('round');
    this.tile = {start: newStart, end: newEnd};
    if (newStart === selection.start && newEnd === selection.end) return;
    this.select(newStart, newEnd, selection.trackUris);
  }

  private select(start: time, end: time, trackUris: ReadonlyArray<string>) {
    this.isUpdatingSelection = true;
    try {
      this.selectionManager.selectArea(
        {start, end, trackUris},
        {clearSearch: false, switchToCurrentSelectionTab: false},
      );
    } finally {
      this.isUpdatingSelection = false;
    }

    // Redrawing the DOM (and so re-running the details panel aggregations)
    // every frame is too slow for smooth panning, so only redraw the canvas
    // and update the details panel once the selection settles.
    raf.scheduleCanvasRedraw();
    this.scheduleDetailsRedraw();
  }

  private spanFor(
    anchor: ViewportAnchor,
    window: HighPrecisionTimeSpan,
  ): HighPrecisionTimeSpan {
    return new HighPrecisionTimeSpan(
      window.start.addNumber(window.duration * anchor.start),
      window.duration * (anchor.end - anchor.start),
    );
  }

  private anchorFor(
    start: HighPrecisionTime,
    end: HighPrecisionTime,
  ): ViewportAnchor {
    const window = this.timeline.visibleWindow;
    const fractionOf = (t: HighPrecisionTime) =>
      t.sub(window.start).toNumber() / window.duration;
    return {start: fractionOf(start), end: fractionOf(end)};
  }

  private setAnchor(area: Area): void {
    this.anchor = this.anchorForArea(area);
    this.tile = {start: area.start, end: area.end};
  }

  private anchorForArea(area: Area): ViewportAnchor {
    return this.anchorFor(
      new HighPrecisionTime(area.start),
      new HighPrecisionTime(area.end),
    );
  }
}
