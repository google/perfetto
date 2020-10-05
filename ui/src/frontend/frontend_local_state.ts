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

import {Actions} from '../common/actions';
import {HttpRpcState} from '../common/http_rpc_engine';
import {
  Area,
  FrontendLocalState as FrontendState,
  OmniboxState,
  Timestamped,
  VisibleState,
} from '../common/state';
import {TimeSpan} from '../common/time';

import {globals} from './globals';
import {debounce, ratelimit} from './rate_limiters';
import {TimeScale} from './time_scale';

interface Range {
  start?: number;
  end?: number;
}

function chooseLatest<T extends Timestamped<{}>>(current: T, next: T): T {
  if (next !== current && next.lastUpdate > current.lastUpdate) {
    return next;
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
  perfDebug = false;
  hoveredUtid = -1;
  hoveredPid = -1;
  hoveredLogsTimestamp = -1;
  hoveredNoteTimestamp = -1;
  highlightedSliceId = -1;
  vidTimestamp = -1;
  localOnlyMode = false;
  sidebarVisible = true;
  showPanningHint = false;
  showCookieConsent = false;
  visibleTracks = new Set<string>();
  prevVisibleTracks = new Set<string>();
  searchIndex = -1;
  currentTab?: string;
  scrollToTrackId?: string|number;
  httpRpcState: HttpRpcState = {connected: false};
  newVersionAvailable = false;

  // This is used to calculate the tracks within a Y range for area selection.
  areaY: Range = {};

  private scrollBarWidth?: number;

  private _omniboxState: OmniboxState = {
    lastUpdate: 0,
    omnibox: '',
    mode: 'SEARCH',
  };

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

  togglePerfDebug() {
    this.perfDebug = !this.perfDebug;
    globals.rafScheduler.scheduleFullRedraw();
  }

  setHoveredUtidAndPid(utid: number, pid: number) {
    this.hoveredUtid = utid;
    this.hoveredPid = pid;
    globals.rafScheduler.scheduleRedraw();
  }

  setHighlightedSliceId(sliceId: number) {
    this.highlightedSliceId = sliceId;
    globals.rafScheduler.scheduleRedraw();
  }

  // Sets the timestamp at which a vertical line will be drawn.
  setHoveredLogsTimestamp(ts: number) {
    if (this.hoveredLogsTimestamp === ts) return;
    this.hoveredLogsTimestamp = ts;
    globals.rafScheduler.scheduleRedraw();
  }

  setHoveredNoteTimestamp(ts: number) {
    if (this.hoveredNoteTimestamp === ts) return;
    this.hoveredNoteTimestamp = ts;
    globals.rafScheduler.scheduleRedraw();
  }

  setVidTimestamp(ts: number) {
    if (this.vidTimestamp === ts) return;
    this.vidTimestamp = ts;
    globals.rafScheduler.scheduleRedraw();
  }

  addVisibleTrack(trackId: string) {
    this.visibleTracks.add(trackId);
  }

  setSearchIndex(index: number) {
    this.searchIndex = index;
    globals.rafScheduler.scheduleRedraw();
  }

  toggleSidebar() {
    this.sidebarVisible = !this.sidebarVisible;
    globals.rafScheduler.scheduleFullRedraw();
  }

  setHttpRpcState(httpRpcState: HttpRpcState) {
    this.httpRpcState = httpRpcState;
    globals.rafScheduler.scheduleFullRedraw();
  }

  // Called when beginning a canvas redraw.
  clearVisibleTracks() {
    this.visibleTracks.clear();
  }

  // Called when the canvas redraw is complete.
  sendVisibleTracks() {
    if (this.prevVisibleTracks.size !== this.visibleTracks.size ||
        ![...this.prevVisibleTracks].every(
            value => this.visibleTracks.has(value))) {
      globals.dispatch(
          Actions.setVisibleTracks({tracks: Array.from(this.visibleTracks)}));
      this.prevVisibleTracks = new Set(this.visibleTracks);
    }
  }

  mergeState(state: FrontendState): void {
    this._omniboxState = chooseLatest(this._omniboxState, state.omniboxState);
    this._visibleState = chooseLatest(this._visibleState, state.visibleState);
    if (this._visibleState === state.visibleState) {
      this.updateLocalTime(
          new TimeSpan(this._visibleState.startSec, this._visibleState.endSec));
    }
  }

  selectArea(
      startSec: number, endSec: number,
      tracks = this._selectedArea ? this._selectedArea.tracks : []) {
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

  private setOmniboxDebounced = debounce(() => {
    globals.dispatch(Actions.setOmnibox(this._omniboxState));
  }, 20);

  setOmnibox(value: string, mode: 'SEARCH'|'COMMAND') {
    this._omniboxState.omnibox = value;
    this._omniboxState.mode = mode;
    this._omniboxState.lastUpdate = Date.now() / 1000;
    this.setOmniboxDebounced();
  }

  get omnibox(): string {
    return this._omniboxState.omnibox;
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
