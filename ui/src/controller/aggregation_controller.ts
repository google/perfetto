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

import {AggregateCpuData} from '../common/aggregation_data';
import {Engine} from '../common/engine';
import {TimestampedAreaSelection} from '../common/state';
import {toNs} from '../common/time';

import {Controller} from './controller';
import {globals} from './globals';

export interface AggregationControllerArgs {
  engine: Engine;
}

export class AggregationController extends Controller<'main'> {
  private previousArea: TimestampedAreaSelection = {lastUpdate: 0};
  private requestingData = false;
  private queuedRequest = false;
  constructor(private args: AggregationControllerArgs) {
    super('main');
  }

  run() {
    const selectedArea = globals.state.frontendLocalState.selectedArea;
    const area = selectedArea.area;
    if (!area ||
        this.previousArea &&
            this.previousArea.lastUpdate >= selectedArea.lastUpdate) {
      return;
    }
    if (this.requestingData) {
      this.queuedRequest = true;
    } else {
      this.requestingData = true;
      Object.assign(this.previousArea, selectedArea);

      this.args.engine.getCpus().then(cpusInTrace => {
        const selectedCpuTracks =
            cpusInTrace.filter(x => area.tracks.includes((x + 1).toString()));

        const query =
            `SELECT process.name, pid, thread.name, tid, sum(dur) AS total_dur,
        count(1)
        FROM process
        JOIN thread USING(upid)
        JOIN thread_state USING(utid)
        WHERE cpu IN (${selectedCpuTracks}) AND
        state = "Running" AND
        thread_state.ts + thread_state.dur > ${toNs(area.startSec)} AND
        thread_state.ts < ${toNs(area.endSec)}
        GROUP BY utid ORDER BY total_dur DESC`;

        this.args.engine.query(query)
            .then(result => {
              if (globals.state.frontendLocalState.selectedArea.lastUpdate >
                  selectedArea.lastUpdate) {
                return;
              }

              const numRows = +result.numRecords;
              const data: AggregateCpuData = {
                strings: [],
                procNameId: new Uint16Array(numRows),
                pid: new Uint32Array(numRows),
                threadNameId: new Uint16Array(numRows),
                tid: new Uint32Array(numRows),
                totalDur: new Float64Array(numRows),
                occurrences: new Uint16Array(numRows)
              };

              const stringIndexes = new Map<string, number>();
              function internString(str: string) {
                let idx = stringIndexes.get(str);
                if (idx !== undefined) return idx;
                idx = data.strings.length;
                data.strings.push(str);
                stringIndexes.set(str, idx);
                return idx;
              }

              for (let row = 0; row < numRows; row++) {
                const cols = result.columns;
                data.procNameId[row] = internString(cols[0].stringValues![row]);
                data.pid[row] = cols[1].longValues![row] as number;
                data.threadNameId[row] =
                    internString(cols[2].stringValues![row]);
                data.tid[row] = cols[3].longValues![row] as number;
                data.totalDur[row] = cols[4].longValues![row] as number;
                data.occurrences[row] = cols[5].longValues![row] as number;
              }
              globals.publish('AggregateCpuData', data);
            })
            .catch(reason => {
              console.error(reason);
            })
            .finally(() => {
              this.requestingData = false;
              if (this.queuedRequest) {
                this.queuedRequest = false;
                this.run();
              }
            });
      });
    }
  }
}