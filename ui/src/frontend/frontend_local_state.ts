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
import {FrontendLocalState as FrontendState} from '../common/state';
import {TimeSpan} from '../common/time';

import {globals} from './globals';
import {TimeScale} from './time_scale';

/**
 * State that is shared between several frontend components, but not the
 * controller. This state is updated at 60fps.
 */
export class FrontendLocalState {
  visibleWindowTime = new TimeSpan(0, 10);
  timeScale = new TimeScale(this.visibleWindowTime, [0, 0]);
  private _lastUpdate = 0;
  private pendingGlobalTimeUpdate = false;
  perfDebug = false;
  hoveredUtid = -1;
  hoveredPid = -1;
  hoveredTimestamp = -1;
  showTimeSelectPreview = false;
  showNotePreview = false;

  // TODO: there is some redundancy in the fact that both |visibleWindowTime|
  // and a |timeScale| have a notion of time range. That should live in one
  // place only.
  updateVisibleTime(ts: TimeSpan) {
    const startSec = Math.max(ts.start, globals.state.traceTime.startSec);
    const endSec = Math.min(ts.end, globals.state.traceTime.endSec);
    this.visibleWindowTime = new TimeSpan(startSec, endSec);
    this.timeScale.setTimeBounds(this.visibleWindowTime);
    this._lastUpdate = Date.now() / 1000;

    // Post a delayed update to the controller.
    if (this.pendingGlobalTimeUpdate) return;
    setTimeout(() => {
      this._lastUpdate = Date.now() / 1000;
      globals.dispatch(Actions.setVisibleTraceTime({
        time: {
          startSec: this.visibleWindowTime.start,
          endSec: this.visibleWindowTime.end,
        },
        lastUpdate: this._lastUpdate,
      }));
      this.pendingGlobalTimeUpdate = false;
    }, 100);
  }

  mergeState(frontendLocalState: FrontendState): void {
    if (this._lastUpdate >= frontendLocalState.lastUpdate) {
      return;
    }
    const visible = frontendLocalState.visibleTraceTime;
    this.updateVisibleTime(new TimeSpan(visible.startSec, visible.endSec));
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

  // Sets the timestamp at which a vertical line will be drawn.
  setHoveredTimestamp(ts: number) {
    if (this.hoveredTimestamp === ts) return;
    this.hoveredTimestamp = ts;
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
}
