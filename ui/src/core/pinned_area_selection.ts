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
import type {TimeSpan} from '../base/time';
import type {Area, Selection, SelectionOpts} from '../public/selection';
import {raf} from './raf_scheduler';

export interface PinnableTimeline {
  readonly visibleWindow: HighPrecisionTimeSpan;
}

export interface PinnableSelectionManager {
  readonly selection: Selection;
  selectArea(area: Area, opts?: SelectionOpts): void;
}

// Zooming never shrinks the selection below this many nanoseconds.
const MIN_SELECTION_DURATION = 1;

// Selection bounds as fractions of the visible window (0 = left edge).
interface ViewportAnchor {
  readonly start: number;
  readonly end: number;
}

/**
 * Pins the area selection to the viewport: panning moves the timeline under
 * the selection, which is re-selected to cover whatever is beneath it. When
 * the timeline is clamped at the trace edge, the selection moves across the
 * screen instead, up to the trace edge. Zooming (see zoom()) resizes the
 * selection rather than the timeline.
 *
 * A new area selection re-pins; any other selection unpins.
 */
export class PinnedAreaSelection {
  private anchor?: ViewportAnchor;
  private isUpdatingSelection = false;

  constructor(
    private readonly timeline: PinnableTimeline,
    private readonly selectionManager: PinnableSelectionManager,
    private readonly traceSpan: TimeSpan,
  ) {}

  get isPinned(): boolean {
    return this.anchor !== undefined;
  }

  pin(): void {
    const selection = this.selectionManager.selection;
    if (selection.kind !== 'area') return;
    this.anchor = this.anchorForArea(selection);
  }

  unpin(): void {
    this.anchor = undefined;
  }

  toggle(): void {
    if (this.isPinned) {
      this.unpin();
    } else {
      this.pin();
    }
  }

  onSelectionChanged(selection: Selection): void {
    // Ignore the selection changes we cause ourselves.
    if (this.isUpdatingSelection) return;
    if (this.anchor === undefined) return;

    if (selection.kind === 'area') {
      this.anchor = this.anchorForArea(selection);
    } else {
      this.anchor = undefined;
    }
  }

  // `requested` is the visible window before clamping to the trace bounds.
  onVisibleWindowChanged(requested: HighPrecisionTimeSpan): void {
    if (this.anchor === undefined) return;
    // Use the requested window so that any movement the timeline couldn't
    // make (due to clamping) moves the selection instead.
    this.updateSelection(this.spanFor(this.anchor, requested));
  }

  // Resizes the selection around its center, leaving the timeline in place.
  // `ratio` < 1 shrinks the selection, > 1 grows it.
  zoom(ratio: number): void {
    if (this.anchor === undefined) return;
    const span = this.spanFor(this.anchor, this.timeline.visibleWindow);
    this.updateSelection(span.scale(ratio, 0.5, MIN_SELECTION_DURATION));
  }

  private updateSelection(requested: HighPrecisionTimeSpan): void {
    const selection = this.selectionManager.selection;
    if (selection.kind !== 'area') {
      this.anchor = undefined;
      return;
    }

    const span = requested.fitWithin(this.traceSpan.start, this.traceSpan.end);
    this.anchor = this.anchorFor(span.start, span.end);

    const newStart = span.start.toTime('round');
    const newEnd = span.end.toTime('round');
    if (newStart === selection.start && newEnd === selection.end) return;

    this.isUpdatingSelection = true;
    try {
      this.selectionManager.selectArea(
        {start: newStart, end: newEnd, trackUris: selection.trackUris},
        {clearSearch: false, switchToCurrentSelectionTab: false},
      );
    } finally {
      this.isUpdatingSelection = false;
    }

    // Window changes only redraw canvases; the details panel is DOM.
    raf.scheduleFullRedraw();
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

  private anchorForArea(area: Area): ViewportAnchor {
    return this.anchorFor(
      new HighPrecisionTime(area.start),
      new HighPrecisionTime(area.end),
    );
  }
}
