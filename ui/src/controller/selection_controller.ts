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

import {Controller} from './controller';
import {globals} from './globals';
import {Engine} from '../common/engine';
import {fromNs} from '../common/time';

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
      const sqlQuery = `SELECT ts, dur, priority, end_state FROM sched
                        WHERE row_id = ${selectedSlice}`;
      this.args.engine.query(sqlQuery).then(result => {
        // Check selection is still the same on completion of query.
        const selection = globals.state.currentSelection;
        if (result.numRecords === 1 &&
            selection &&
            selection.kind === 'SLICE' &&
            selection.id === selectedSlice) {
          const ts = fromNs(result.columns[0].longValues![0] as number);
          const timeFromStart = ts - globals.state.traceTime.startSec;
          const dur = fromNs(result.columns[1].longValues![0] as number);
          const priority = result.columns[2].longValues![0] as number;
          const endState = result.columns[3].stringValues![0];
          const selected = {ts: timeFromStart, dur, priority, endState};
          globals.publish('SliceDetails', selected);
        }
      });
    }
  }
}
