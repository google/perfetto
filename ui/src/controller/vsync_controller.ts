// Copyright (C) 2022 The Android Open Source Project
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

import {assertExists} from '../base/logging';
import {Engine} from '../common/engine';
import {LONG, NUM} from '../common/query_result';
import {Span} from '../common/time';
import {globals} from '../frontend/globals';
import {publishVsyncData} from '../frontend/publish';
import {Controller} from './controller';

export interface VsyncControllerArgs {
  engine: Engine;
}

export class VsyncController extends Controller<'init'|'ready'> {
  private engine: Engine;
  private trackId?: number;
  private timeSpan?: Span<bigint>;

  constructor(args: VsyncControllerArgs) {
    super('init');

    this.engine = args.engine;
  }

  onDestroy() {
    publishVsyncData({});
  }

  run() {
    switch (this.state) {
      case 'init':
        this.getVsyncTrack().then((trackId) => this.trackId = trackId);
        this.setState('ready');
        break;
      case 'ready':
        if (this.trackId === undefined) {
          // Still waiting for the track ID
          return;
        }
        const visibleTimeSpan = globals.stateVisibleTime();
        if (this.timeSpan === undefined ||
            !this.timeSpan.equals(visibleTimeSpan)) {
          this.timeSpan = visibleTimeSpan;
          this.loadVsyncData(this.trackId, this.timeSpan);
        }
        break;
      default:
        throw new Error(`Unexpected state ${this.state}`);
    }
  }

  async getVsyncTrack(): Promise<number|undefined> {
    // Determine the track ID of the SurfaceFlinger VSYNC-app counter, if
    // it exists.
    const result = await this.engine.query(`
      select process_counter_track.id as trackId
      from process
        join process_counter_track using (upid)
      where process.name like '%/surfaceflinger' and
        process_counter_track.name='VSYNC-app'
      limit 1;
    `);
    if (result.numRows() < 1) {
      return undefined;
    }

    return result.firstRow({'trackId': NUM}).trackId;
  }

  async loadVsyncData(trackId: number, timeSpan: Span<bigint>) {
    // Try to get at least two changes of the counter, even if that means
    // reaching beyond the currently visible timespan of the trace.
    // But in any case get all changes of the counter that are in that
    // visible span. Depending on the trace, and when zoomed in tight
    // towards the end of the trace, there may not even be as many as
    // two counter events to retrieve, so in that case we'll just
    // get what we can.
    const result = await this.engine.query(`
      select ts, value from (
        select row_number() over (order by ts) as rn, ts, value
        from counter
        where track_id = ${trackId} and ts >= ${timeSpan.start}
        order by ts)
      where rn <= 2 or ts <= ${timeSpan.end};
    `);

    const toggleTs: bigint[] = [];
    let initiallyOn: boolean|undefined;
    let lastWasOn = false;

    const row = result.iter({ts: LONG, value: NUM});
    for (; row.valid(); row.next()) {
      const on = (row.value === 1);
      if (initiallyOn === undefined) {
        initiallyOn = !on;
        lastWasOn = initiallyOn;
      }
      if (on !== lastWasOn) {
        lastWasOn = on;
        toggleTs.push(row.ts);
      } // Otherwise, it didn't toggle
    }

    if (toggleTs.length === 0) {
      publishVsyncData({});
    } else {
      initiallyOn = assertExists(initiallyOn);
      publishVsyncData({initiallyOn, toggleTs});
    }
  }
}
