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
import {
  FrontendLocalState as FrontendState,
  OmniboxState,
  Timestamped,
  VisibleState
} from '../common/state';
import {TimeSpan} from '../common/time';

import {globals} from './globals';
import {TimeScale} from './time_scale';

function chooseLastest<T extends Timestamped<{}>>(current: T, next: T): T {
  if (next !== current && next.lastUpdate > current.lastUpdate) {
    return next;
  }
  return current;
}

// Returns a wrapper around |f| which calls f at most once every |ms|ms.
function ratelimit(f: Function, ms: number): Function {
  let inProgess = false;
  return () => {
    if (inProgess) {
      return;
    }
    inProgess = true;
    window.setTimeout(() => {
      f();
      inProgess = false;
    }, ms);
  };
}

// Returns a wrapper around |f| which waits for a |ms|ms pause in calls
// before calling |f|.
function debounce(f: Function, ms: number): Function {
  let timerId: undefined|number;
  return () => {
    if (timerId) {
      window.clearTimeout(timerId);
    }
    timerId = window.setTimeout(() => {
      f();
      timerId = undefined;
    }, ms);
  };
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
  hoveredTimestamp = -1;
  vidTimestamp = -1;
  showTimeSelectPreview = false;
  showNotePreview = false;
  localOnlyMode = false;
  sidebarVisible = true;
  visibleTracks = new Set<string>();
  prevVisibleTracks = new Set<string>();

  private _omniboxState: OmniboxState = {
    lastUpdate: 0,
    omnibox: '',
  };

  private _visibleState: VisibleState = {
    lastUpdate: 0,
    startSec: 0,
    endSec: 10,
    resolution: 1,
  };

  // TODO: there is some redundancy in the fact that both |visibleWindowTime|
  // and a |timeScale| have a notion of time range. That should live in one
  // place only.

  togglePerfDebug() {
    this.perfDebug = !this.perfDebug;
    globals.rafScheduler.scheduleFullRedraw();
  }

  setHoveredUtidAndPid(utid: number, pid: number) {
    this.hoveredUtid = utid;
    this.hoveredPid = pid;
    globals.rafScheduler.scheduleRedraw();
  }

  // Sets the timestamp at which a vertical line will be drawn.
  setHoveredTimestamp(ts: number) {
    if (this.hoveredTimestamp === ts) return;
    this.hoveredTimestamp = ts;
    globals.rafScheduler.scheduleRedraw();
  }

  setVidTimestamp(ts: number) {
    if (this.vidTimestamp === ts) return;
    this.vidTimestamp = ts;
    globals.rafScheduler.scheduleRedraw();
  }

  setShowNotePreview(show: boolean) {
    this.showNotePreview = show;
    globals.rafScheduler.scheduleRedraw();
  }

  setShowTimeSelectPreview(show: boolean) {
    this.showTimeSelectPreview = show;
    globals.rafScheduler.scheduleRedraw();
  }

  addVisibleTrack(trackId: string) {
    this.visibleTracks.add(trackId);
  }

  toggleSidebar() {
    this.sidebarVisible = !this.sidebarVisible;
    globals.rafScheduler.scheduleFullRedraw();
  }

  // Called when beginning a canvas redraw.
  clearVisibleTracks() {
    this.prevVisibleTracks = new Set(this.visibleTracks);
    this.visibleTracks.clear();
  }

  // Called when the canvas redraw is complete.
  sendVisibleTracks() {
    if (this.prevVisibleTracks.size !== this.visibleTracks.size ||
        ![...this.prevVisibleTracks].every(
            value => this.visibleTracks.has(value))) {
      globals.dispatch(
          Actions.setVisibleTracks({tracks: Array.from(this.visibleTracks)}));
    }
  }

  mergeState(state: FrontendState): void {
    this._omniboxState = chooseLastest(this._omniboxState, state.omniboxState);
    this._visibleState = chooseLastest(this._visibleState, state.visibleState);
    this.updateLocalTime(
        new TimeSpan(this._visibleState.startSec, this._visibleState.endSec));
  }

  private debouncedSetOmnibox = debounce(() => {
    globals.dispatch(Actions.setOmnibox(this._omniboxState));
  }, 20);

  set omnibox(value: string) {
    this._omniboxState.omnibox = value;
    this._omniboxState.lastUpdate = Date.now() / 1000;
    this.debouncedSetOmnibox();
  }

  get omnibox(): string {
    return this._omniboxState.omnibox;
  }

  private ratelimitedUpdateVisible = ratelimit(() => {
    globals.dispatch(Actions.setVisibleTraceTime(this._visibleState));
  }, 50);

  private updateLocalTime(ts: TimeSpan) {
    const startSec = Math.max(ts.start, globals.state.traceTime.startSec);
    const endSec = Math.min(ts.end, globals.state.traceTime.endSec);
    this.visibleWindowTime = new TimeSpan(startSec, endSec);
    this.timeScale.setTimeBounds(this.visibleWindowTime);
  }

  updateVisibleTime(ts: TimeSpan) {
    this.updateLocalTime(ts);
    this._visibleState.lastUpdate = Date.now() / 1000;
    this._visibleState.startSec = this.visibleWindowTime.start;
    this._visibleState.endSec = this.visibleWindowTime.end;
    this._visibleState.resolution = globals.getCurResolution();
    this.ratelimitedUpdateVisible();
  }
}
