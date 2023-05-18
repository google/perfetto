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
import {Actions} from '../common/actions';
import {
  HighPrecisionTime,
  HighPrecisionTimeSpan,
} from '../common/high_precision_time';
import {HttpRpcState} from '../common/http_rpc_engine';
import {
  Area,
  FrontendLocalState as FrontendState,
  Timestamped,
  VisibleState,
} from '../common/state';
import {Span} from '../common/time';
import {
  TPTime,
  TPTimeSpan,
} from '../common/time';

import {globals} from './globals';
import {ratelimit} from './rate_limiters';
import {PxSpan, TimeScale} from './time_scale';

interface Range {
  start?: number;
  end?: number;
}

function chooseLatest<T extends Timestamped>(current: T, next: T): T {
  if (next !== current && next.lastUpdate > current.lastUpdate) {
    // |next| is from state. Callers may mutate the return value of
    // this function so we need to clone |next| to prevent bad mutations
    // of state:
    return Object.assign({}, next);
  }
  return current;
}

// Calculate the space a scrollbar takes up so that we can subtract it from
// the canvas width.
function calculateScrollbarWidth() {
  const outer = document.createElement('div');
  outer.style.overflowY = 'scroll';
  const inner = document.createElement('div');
  outer.appendChild(inner);
  document.body.appendChild(outer);
  const width =
      outer.getBoundingClientRect().width - inner.getBoundingClientRect().width;
  document.body.removeChild(outer);
  return width;
}

export class TimeWindow {
  private readonly MIN_DURATION_NS = 10;
  private _start: HighPrecisionTime = new HighPrecisionTime();
  private _durationNanos: number = 10e9;

  private get _end(): HighPrecisionTime {
    return this._start.addNanos(this._durationNanos);
  }

  update(span: Span<HighPrecisionTime>) {
    this._start = span.start;
    this._durationNanos = Math.max(this.MIN_DURATION_NS, span.duration.nanos);
    this.preventClip();
  }

  // Pan the window by certain number of seconds
  pan(offset: HighPrecisionTime) {
    this._start = this._start.add(offset);
    this.preventClip();
  }

  // Zoom in or out a bit centered on a specific offset from the root
  // Offset represents the center of the zoom as a normalized value between 0
  // and 1 where 0 is the start of the time window and 1 is the end
  zoom(ratio: number, offset: number) {
    // TODO(stevegolton): Handle case where trace time < MIN_DURATION_NS

    const traceDuration = globals.stateTraceTime().duration;
    const minDuration = Math.min(this.MIN_DURATION_NS, traceDuration.nanos);
    const newDurationNanos = Math.max(this._durationNanos * ratio, minDuration);
    // Delta between new and old duration
    // +ve if new duration is shorter than old duration
    const durationDeltaNanos = this._durationNanos - newDurationNanos;
    // If offset is 0, don't move the start at all
    // If offset if 1, move the start by the amount the duration has changed
    // If new duration is shorter - move start to right
    // If new duration is longer - move start to left
    this._start = this._start.addNanos(durationDeltaNanos * offset);
    this._durationNanos = newDurationNanos;
    this.preventClip();
  }

  createTimeScale(startPx: number, endPx: number): TimeScale {
    return new TimeScale(
        this._start, this._durationNanos, new PxSpan(startPx, endPx));
  }

  // Get timespan covering entire range of the window
  get timeSpan(): HighPrecisionTimeSpan {
    return new HighPrecisionTimeSpan(this._start, this._end);
  }

  get timestampSpan(): Span<TPTime> {
    return new TPTimeSpan(this.earliest, this.latest);
  }

  get earliest(): TPTime {
    return this._start.toTPTime('floor');
  }

  get latest(): TPTime {
    return this._start.addNanos(this._durationNanos).toTPTime('ceil');
  }

  // Limit the zoom and pan
  private preventClip() {
    const traceTimeSpan = globals.stateTraceTime();
    const traceDurationNanos = traceTimeSpan.duration.nanos;

    if (this._durationNanos > traceDurationNanos) {
      this._start = traceTimeSpan.start;
      this._durationNanos = traceDurationNanos;
    }

    if (this._start.isLessThan(traceTimeSpan.start)) {
      this._start = traceTimeSpan.start;
    }

    const end = this._start.addNanos(this._durationNanos);
    if (end.isGreaterThan(traceTimeSpan.end)) {
      this._start = traceTimeSpan.end.subtractNanos(this._durationNanos);
    }
  }
}

/**
 * State that is shared between several frontend components, but not the
 * controller. This state is updated at 60fps.
 */
export class FrontendLocalState {
  visibleWindow = new TimeWindow();
  startPx: number = 0;
  endPx: number = 0;
  showPanningHint = false;
  showCookieConsent = false;
  visibleTracks = new Set<string>();
  prevVisibleTracks = new Set<string>();
  scrollToTrackId?: string|number;
  httpRpcState: HttpRpcState = {connected: false};
  newVersionAvailable = false;

  // This is used to calculate the tracks within a Y range for area selection.
  areaY: Range = {};

  private scrollBarWidth?: number;

  private _visibleState: VisibleState = {
    lastUpdate: 0,
    start: 0n,
    end: BigInt(10e9),
    resolution: 1n,
  };

  private _selectedArea?: Area;

  // TODO: there is some redundancy in the fact that both |visibleWindowTime|
  // and a |timeScale| have a notion of time range. That should live in one
  // place only.

  getScrollbarWidth() {
    if (this.scrollBarWidth === undefined) {
      this.scrollBarWidth = calculateScrollbarWidth();
    }
    return this.scrollBarWidth;
  }

  setHttpRpcState(httpRpcState: HttpRpcState) {
    this.httpRpcState = httpRpcState;
    globals.rafScheduler.scheduleFullRedraw();
  }

  addVisibleTrack(trackId: string) {
    this.visibleTracks.add(trackId);
  }

  // Called when beginning a canvas redraw.
  clearVisibleTracks() {
    this.visibleTracks.clear();
  }

  // Called when the canvas redraw is complete.
  sendVisibleTracks() {
    if (this.prevVisibleTracks.size !== this.visibleTracks.size ||
        ![...this.prevVisibleTracks].every(
            (value) => this.visibleTracks.has(value))) {
      globals.dispatch(
          Actions.setVisibleTracks({tracks: Array.from(this.visibleTracks)}));
      this.prevVisibleTracks = new Set(this.visibleTracks);
    }
  }

  zoomVisibleWindow(ratio: number, centerPoint: number) {
    this.visibleWindow.zoom(ratio, centerPoint);
    this.kickUpdateLocalState();
  }

  panVisibleWindow(delta: HighPrecisionTime) {
    this.visibleWindow.pan(delta);
    this.kickUpdateLocalState();
  }

  mergeState(state: FrontendState): void {
    // This is unfortunately subtle. This class mutates this._visibleState.
    // Since we may not mutate |state| (in order to make immer's immutable
    // updates work) this means that we have to make a copy of the visibleState.
    // when updating it. We don't want to have to do that unnecessarily so
    // chooseLatest returns a shallow clone of state.visibleState *only* when
    // that is the newer state. All of these complications should vanish when
    // we remove this class.
    const previousVisibleState = this._visibleState;
    this._visibleState = chooseLatest(this._visibleState, state.visibleState);
    const visibleStateWasUpdated = previousVisibleState !== this._visibleState;
    if (visibleStateWasUpdated) {
      this.updateLocalTime(new HighPrecisionTimeSpan(
          HighPrecisionTime.fromTPTime(this._visibleState.start),
          HighPrecisionTime.fromTPTime(this._visibleState.end),
          ));
    }
  }

  // Set the highlight box to draw
  selectArea(
      start: TPTime, end: TPTime,
      tracks = this._selectedArea ? this._selectedArea.tracks : []) {
    assertTrue(
        end >= start,
        `Impossible select area: start [${start}] >= end [${end}]`);
    this.showPanningHint = true;
    this._selectedArea = {start, end, tracks},
    globals.rafScheduler.scheduleFullRedraw();
  }

  deselectArea() {
    this._selectedArea = undefined;
    globals.rafScheduler.scheduleRedraw();
  }

  get selectedArea(): Area|undefined {
    return this._selectedArea;
  }

  private ratelimitedUpdateVisible = ratelimit(() => {
    globals.dispatch(Actions.setVisibleTraceTime(this._visibleState));
  }, 50);

  private updateLocalTime(ts: Span<HighPrecisionTime>) {
    const traceBounds = globals.stateTraceTime();
    const start = ts.start.clamp(traceBounds.start, traceBounds.end);
    const end = ts.end.clamp(traceBounds.start, traceBounds.end);
    this.visibleWindow.update(new HighPrecisionTimeSpan(start, end));
    this.updateResolution();
  }

  private updateResolution() {
    this._visibleState.lastUpdate = Date.now() / 1000;
    this._visibleState.resolution = globals.getCurResolution();
    this.ratelimitedUpdateVisible();
  }

  private kickUpdateLocalState() {
    this._visibleState.lastUpdate = Date.now() / 1000;
    this._visibleState.start = this.visibleWindowTime.start.toTPTime();
    this._visibleState.end = this.visibleWindowTime.end.toTPTime();
    this._visibleState.resolution = globals.getCurResolution();
    this.ratelimitedUpdateVisible();
  }

  updateVisibleTime(ts: Span<HighPrecisionTime>) {
    this.updateLocalTime(ts);
    this.kickUpdateLocalState();
  }

  // Whenever start/end px of the timeScale is changed, update
  // the resolution.
  updateLocalLimits(pxStart: number, pxEnd: number) {
    // Numbers received here can be negative or equal, but we should fix that
    // before updating the timescale.
    pxStart = Math.max(0, pxStart);
    pxEnd = Math.max(0, pxEnd);
    if (pxStart === pxEnd) pxEnd = pxStart + 1;
    this.startPx = pxStart;
    this.endPx = pxEnd;
    this.updateResolution();
  }

  // Get the time scale for the visible window
  get visibleTimeScale(): TimeScale {
    return this.visibleWindow.createTimeScale(this.startPx, this.endPx);
  }

  // Produces a TimeScale object for this time window provided start and end px
  getTimeScale(startPx: number, endPx: number): TimeScale {
    return this.visibleWindow.createTimeScale(startPx, endPx);
  }

  // Get the bounds of the window in pixels
  get windowSpan(): PxSpan {
    return new PxSpan(this.startPx, this.endPx);
  }

  // Get the bounds of the visible time window as a time span
  get visibleWindowTime(): Span<HighPrecisionTime> {
    return this.visibleWindow.timeSpan;
  }
}
