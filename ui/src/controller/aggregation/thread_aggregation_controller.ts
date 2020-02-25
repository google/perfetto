// Copyright (C) 2020 The Android Open Source Project
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

import {AggregateData} from '../../common/aggregation_data';
import {Engine} from '../../common/engine';
import {TimestampedAreaSelection} from '../../common/state';
import {translateState} from '../../common/thread_state';
import {toNs} from '../../common/time';
import {
  Config,
  THREAD_STATE_TRACK_KIND
} from '../../tracks/thread_state/common';
import {globals} from '../globals';

import {AggregationController} from './aggregation_controller';


export class ThreadAggregationController extends AggregationController {
  async onAreaSelectionChange(
      engine: Engine, selectedArea: TimestampedAreaSelection) {
    const area = selectedArea.area;
    if (area === undefined) {
      return {columns: [], strings: []};
    }
    // TODO(taylori): Thread state tracks should have a real track id in the
    // trace processor.
    const utids = [];
    for (const trackId of area.tracks) {
      const track = globals.state.tracks[trackId];
      if (track.kind === THREAD_STATE_TRACK_KIND) {
        utids.push((track.config as Config).utid);
      }
    }

    const query = `SELECT process.name, pid, thread.name, tid,
      state,
      sum(dur) AS total_dur,
      sum(dur)/count(1) as avg_dur,
      count(1) as occurences
      FROM process
      JOIN thread USING(upid)
      JOIN thread_state USING(utid)
      WHERE utid IN (${utids}) AND
      thread_state.ts + thread_state.dur > ${toNs(area.startSec)} AND
      thread_state.ts < ${toNs(area.endSec)}
      GROUP BY utid, state ORDER BY total_dur DESC`;

    const result = await engine.query(query);

    const numRows = +result.numRecords;
    const aggregateData: AggregateData = {
      columns: [
        {title: 'Process', kind: 'STRING', data: new Uint16Array(numRows)},
        {title: 'PID', kind: 'NUMBER', data: new Uint16Array(numRows)},
        {title: 'Thread', kind: 'STRING', data: new Uint16Array(numRows)},
        {title: 'TID', kind: 'NUMBER', data: new Uint16Array(numRows)},
        {title: 'State', kind: 'STRING', data: new Uint16Array(numRows)},
        {
          title: 'Wall duration (ms)',
          kind: 'TIMESTAMP_NS',
          data: new Float64Array(numRows)
        },
        {
          title: 'Avg Wall duration (ms)',
          kind: 'TIMESTAMP_NS',
          data: new Float64Array(numRows)
        },
        {title: 'Occurrences', kind: 'NUMBER', data: new Uint16Array(numRows)}
      ],
      strings: [],
    };

    const stringIndexes = new Map<string, number>();
    function internString(str: string) {
      let idx = stringIndexes.get(str);
      if (idx !== undefined) return idx;
      idx = aggregateData.strings.length;
      aggregateData.strings.push(str);
      stringIndexes.set(str, idx);
      return idx;
    }

    for (let row = 0; row < numRows; row++) {
      const cols = result.columns;
      aggregateData.columns[0].data[row] =
          internString(cols[0].stringValues![row]);
      aggregateData.columns[1].data[row] = cols[1].longValues![row] as number;
      aggregateData.columns[2].data[row] =
          internString(cols[2].stringValues![row]);
      aggregateData.columns[3].data[row] = cols[3].longValues![row] as number;
      aggregateData.columns[4].data[row] =
          internString(translateState(cols[4].stringValues![row]));
      aggregateData.columns[5].data[row] = cols[5].longValues![row] as number;
      aggregateData.columns[6].data[row] = cols[6].longValues![row] as number;
      aggregateData.columns[7].data[row] = cols[7].longValues![row] as number;
    }
    return aggregateData;
  }
}
