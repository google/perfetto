// Copyright (C) 2018 The Android Open Source Project
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

import {assertTrue} from '../base/logging';
import {time, Span, duration} from '../base/time';
import {HighPrecisionTimeSpan} from '../common/high_precision_time_span';
import {Area, State} from '../common/state';
import {raf} from '../core/raf_scheduler';
import {Store} from '../public';

import {ratelimit} from './rate_limiters';
import {PxSpan, TimeScale} from './time_scale';

interface Range {
  start?: number;
  end?: number;
}

const MIN_DURATION = 10;

/**
 * State that is shared between several frontend components, but not the
 * controller. This state is updated at 60fps.
 */
export class Timeline {
  private _visibleWindow: HighPrecisionTimeSpan;
  private _timeScale: TimeScale;
  private _windowSpan = PxSpan.ZERO;
  private readonly traceSpan;
  private readonly store;

  // This is used to calculate the tracks within a Y range for area selection.
  areaY: Range = {};
  private _selectedArea?: Area;

  constructor(store: Store<State>, traceSpan: Span<time, duration>) {
    this.store = store;
    this.traceSpan = traceSpan;
    this._visibleWindow = HighPrecisionTimeSpan.fromTime(
      traceSpan.start,
      traceSpan.end,
    );
    this._timeScale = new TimeScale(this._visibleWindow, new PxSpan(0, 0));
  }

  // This is a giant hack. Basically, removing visible window from the state
  // means that we no longer update the state periodically while navigating
  // the timeline, which means that controllers are not running. This keeps
  // making null edits to the store which triggers the controller to run.
  //
  // TODO(stevegolton): When we remove controllers, we can remove this!
  private readonly rateLimitedPoker = ratelimit(
    () => this.store.edit(() => {}),
    50,
  );

  // TODO: there is some redundancy in the fact that both |visibleWindowTime|
  // and a |timeScale| have a notion of time range. That should live in one
  // place only.

  zoomVisibleWindow(ratio: number, centerPoint: number) {
    this._visibleWindow = this._visibleWindow
      .scale(ratio, centerPoint, MIN_DURATION)
      .fitWithin(this.traceSpan.start, this.traceSpan.end);

    this.updateTimeScale();
    this.rateLimitedPoker();
  }

  panVisibleWindow(delta: number) {
    this._visibleWindow = this._visibleWindow
      .translate(delta)
      .fitWithin(this.traceSpan.start, this.traceSpan.end);
    this.updateTimeScale();
    this.rateLimitedPoker();
  }

  // Set the highlight box to draw
  selectArea(
    start: time,
    end: time,
    tracks = this._selectedArea ? this._selectedArea.tracks : [],
  ) {
    assertTrue(
      end >= start,
      `Impossible select area: start [${start}] >= end [${end}]`,
    );
    this._selectedArea = {start, end, tracks};
    raf.scheduleFullRedraw();
  }

  deselectArea() {
    this._selectedArea = undefined;
    raf.scheduleRedraw();
  }

  get selectedArea(): Area | undefined {
    return this._selectedArea;
  }

  // Set visible window using an integer time span
  updateVisibleTime(ts: Span<time, duration>) {
    this.updateVisibleTimeHP(HighPrecisionTimeSpan.fromTime(ts.start, ts.end));
  }

  // Set visible window using a high precision time span
  updateVisibleTimeHP(ts: HighPrecisionTimeSpan) {
    this._visibleWindow = ts.intersect(
      this.traceSpan.start,
      this.traceSpan.end,
    );
    this.updateTimeScale();
    this.rateLimitedPoker();
  }

  private updateTimeScale(): void {
    this._timeScale = new TimeScale(this._visibleWindow, this._windowSpan);
  }

  updateLocalLimits(pxStart: number, pxEnd: number) {
    // Numbers received here can be negative or equal, but we should fix that
    // before updating the timescale.
    pxStart = Math.max(0, pxStart);
    pxEnd = Math.max(0, pxEnd);
    if (pxStart === pxEnd) pxEnd = pxStart + 1;
    this._windowSpan = new PxSpan(pxStart, pxEnd);
    this.updateTimeScale();
  }

  // Get the time scale for the visible window
  get visibleTimeScale(): TimeScale {
    return this._timeScale;
  }

  // Produces a TimeScale object for this time window provided start and end px
  getTimeScale(startPx: number, endPx: number): TimeScale {
    return new TimeScale(this._visibleWindow, new PxSpan(startPx, endPx));
  }

  // Get the bounds of the window in pixels
  get windowSpan(): PxSpan {
    return this._windowSpan;
  }

  // Get the bounds of the visible window as a high-precision time span
  get visibleWindowTime(): HighPrecisionTimeSpan {
    return this._visibleWindow;
  }

  get visibleTimeSpan(): Span<time, duration> {
    return this._visibleWindow.toTimeSpan();
  }
}
