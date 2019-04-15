// Copyright (C) 2019 The Android Open Source Project
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

import {Engine} from '../common/engine';
import {fromNs} from '../common/time';
import {SliceDetails} from '../frontend/globals';

import {Controller} from './controller';
import {globals} from './globals';

export interface SelectionControllerArgs {
  engine: Engine;
}

// This class queries the TP for the details on a specific slice that has
// been clicked.
export class SelectionController extends Controller<'main'> {
  private lastSelectedSlice?: number;
  constructor(private args: SelectionControllerArgs) {
    super('main');
  }

  run() {
    const selection = globals.state.currentSelection;
    if (selection === null ||
        selection.kind !== 'SLICE' ||
        selection.id === this.lastSelectedSlice) {
      return;
    }
    const selectedSlice = selection.id;
    this.lastSelectedSlice = selectedSlice;

    if (selectedSlice !== undefined) {
      const sqlQuery = `SELECT ts, dur, priority, end_state, utid FROM sched
                        WHERE row_id = ${selectedSlice}`;
      this.args.engine.query(sqlQuery).then(result => {
        // Check selection is still the same on completion of query.
        const selection = globals.state.currentSelection;
        if (result.numRecords === 1 &&
            selection &&
            selection.kind === 'SLICE' &&
            selection.id === selectedSlice) {
          const ts = result.columns[0].longValues![0] as number;
          const timeFromStart = fromNs(ts) - globals.state.traceTime.startSec;
          const dur = fromNs(result.columns[1].longValues![0] as number);
          const priority = result.columns[2].longValues![0] as number;
          const endState = result.columns[3].stringValues![0];
          const selected:
              SliceDetails = {ts: timeFromStart, dur, priority, endState};
          const utid = result.columns[4].longValues![0];
          this.schedulingDetails(ts, utid).then(wakeResult => {
            Object.assign(selected, wakeResult);
            globals.publish('SliceDetails', selected);
          });
        }
      });
    }
  }

  async schedulingDetails(ts: number, utid: number|Long) {
    // Find the ts of the first sched_wakeup before the current slice.
    const queryWakeupTs = `select ts from instants where name = 'sched_wakeup'
    and ref = ${utid} and ts < ${ts} order by ts desc limit 1`;
    const wakeupRow = await this.args.engine.queryOneRow(queryWakeupTs);
    // Find the previous sched slice for the current utid.
    const queryPrevSched = `select ts from sched where utid = ${utid}
    and ts < ${ts} order by ts desc limit 1`;
    const prevSchedRow = await this.args.engine.queryOneRow(queryPrevSched);
    // If this is the first sched slice for this utid or if the wakeup found
    // was after the previous slice then we know the wakeup was for this slice.
    if (prevSchedRow[0] && wakeupRow[0] < prevSchedRow[0]) {
      return undefined;
    }
    const wakeupTs = wakeupRow[0];
    // Find the sched slice with the utid of the waker running when the
    // sched wakeup occurred. This is the waker.
    const queryWaker = `select utid, cpu from sched where utid =
    (select utid from raw where name = 'sched_wakeup' and ts = ${wakeupTs})
    and ts < ${wakeupTs} and ts + dur >= ${wakeupTs};`;
    const wakerRow = await this.args.engine.queryOneRow(queryWaker);
    if (wakerRow) {
      return {
        wakeupTs: fromNs(wakeupTs),
        wakerUtid: wakerRow[0],
        wakerCpu: wakerRow[1]
      };
    } else {
      return undefined;
    }
  }
}
