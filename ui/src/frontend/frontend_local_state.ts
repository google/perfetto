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

import {setVisibleTraceTime} from '../common/actions';
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
  private _visibleTimeLastUpdate = 0;
  private pendingGlobalTimeUpdate?: TimeSpan;

  // TODO: there is some redundancy in the fact that both |visibleWindowTime|
  // and a |timeScale| have a notion of time range. That should live in one
  // place only.
  updateVisibleTime(ts: TimeSpan) {
    const startSec = Math.max(ts.start, globals.state.traceTime.startSec);
    const endSec = Math.min(ts.end, globals.state.traceTime.endSec);
    this.visibleWindowTime = new TimeSpan(startSec, endSec);
    this.timeScale.setTimeBounds(this.visibleWindowTime);
    this._visibleTimeLastUpdate = Date.now() / 1000;

    // Post a delayed update to the controller.
    const alreadyPosted = this.pendingGlobalTimeUpdate !== undefined;
    this.pendingGlobalTimeUpdate = this.visibleWindowTime;
    if (alreadyPosted) return;
    setTimeout(() => {
      globals.dispatch(setVisibleTraceTime(this.pendingGlobalTimeUpdate!));
      this._visibleTimeLastUpdate = Date.now() / 1000;
      this.pendingGlobalTimeUpdate = undefined;
    }, 100);
  }

  get visibleTimeLastUpdate() {
    return this._visibleTimeLastUpdate;
  }
}
