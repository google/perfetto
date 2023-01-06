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
import {HttpRpcState} from '../common/http_rpc_engine';
import {
  Area,
  FrontendLocalState as FrontendState,
  Timestamped,
  VisibleState,
} from '../common/state';
import {TimeSpan} from '../common/time';

import {globals} from './globals';
import {ratelimit} from './rate_limiters';
import {TimeScale} from './time_scale';

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

function capBetween(t: number, start: number, end: number) {
  return Math.min(Math.max(t, start), end);
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

/**
 * State that is shared between several frontend components, but not the
 * controller. This state is updated at 60fps.
 */
export class FrontendLocalState {
  visibleWindowTime = new TimeSpan(0, 10);
  timeScale = new TimeScale(this.visibleWindowTime, [0, 0]);
  showPanningHint = false;
  showCookieConsent = false;
  visibleTracks = new Set<string>();
  prevVisibleTracks = new Set<string>();
  scrollToTrackId?: string|number;
  httpRpcState: HttpRpcState = {connected: false};
  newVersionAvailable = false;
  showPivotTable = false;

  // This is used to calculate the tracks within a Y range for area selection.
  areaY: Range = {};

  private scrollBarWidth?: number;

  private _visibleState: VisibleState = {
    lastUpdate: 0,
    startSec: 0,
    endSec: 10,
    resolution: 1,
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

  togglePivotTable() {
    this.showPivotTable = !this.showPivotTable;
    globals.rafScheduler.scheduleFullRedraw();
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
      this.updateLocalTime(
          new TimeSpan(this._visibleState.startSec, this._visibleState.endSec));
    }
  }

  selectArea(
      startSec: number, endSec: number,
      tracks = this._selectedArea ? this._selectedArea.tracks : []) {
    assertTrue(endSec >= startSec);
    this.showPanningHint = true;
    this._selectedArea = {startSec, endSec, tracks},
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

  private updateLocalTime(ts: TimeSpan) {
    const traceTime = globals.state.traceTime;
    const startSec = capBetween(ts.start, traceTime.startSec, traceTime.endSec);
    const endSec = capBetween(ts.end, traceTime.startSec, traceTime.endSec);
    this.visibleWindowTime = new TimeSpan(startSec, endSec);
    this.timeScale.setTimeBounds(this.visibleWindowTime);
    this.updateResolution();
  }

  private updateResolution() {
    this._visibleState.lastUpdate = Date.now() / 1000;
    this._visibleState.resolution = globals.getCurResolution();
    this.ratelimitedUpdateVisible();
  }

  updateVisibleTime(ts: TimeSpan) {
    this.updateLocalTime(ts);
    this._visibleState.lastUpdate = Date.now() / 1000;
    this._visibleState.startSec = this.visibleWindowTime.start;
    this._visibleState.endSec = this.visibleWindowTime.end;
    this._visibleState.resolution = globals.getCurResolution();
    this.ratelimitedUpdateVisible();
  }

  getVisibleStateBounds(): [number, number] {
    return [this.visibleWindowTime.start, this.visibleWindowTime.end];
  }

  // Whenever start/end px of the timeScale is changed, update
  // the resolution.
  updateLocalLimits(pxStart: number, pxEnd: number) {
    // Numbers received here can be negative or equal, but we should fix that
    // before updating the timescale.
    pxStart = Math.max(0, pxStart);
    pxEnd = Math.max(0, pxEnd);
    if (pxStart === pxEnd) pxEnd = pxStart + 1;
    this.timeScale.setLimitsPx(pxStart, pxEnd);
    this.updateResolution();
  }
}
