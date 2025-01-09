// Copyright (C) 2024 The Android Open Source Project
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

import {Time} from '../../base/time';
import {
  BaseCounterTrack,
  CounterOptions,
} from '../../components/tracks/base_counter_track';
import {TrackEventDetails} from '../../public/selection';
import {Trace} from '../../public/trace';
import {TrackMouseEvent} from '../../public/track';
import {LONG, LONG_NULL, NUM} from '../../trace_processor/query_result';
import {CounterDetailsPanel} from './counter_details_panel';

export class TraceProcessorCounterTrack extends BaseCounterTrack {
  constructor(
    trace: Trace,
    uri: string,
    options: Partial<CounterOptions>,
    private readonly trackId: number,
    private readonly trackName: string,
    private readonly rootTable: string = 'counter',
  ) {
    super(trace, uri, options);
  }

  getSqlSource() {
    return `
      select
        id,
        ts,
        value
      from ${this.rootTable}
      where track_id = ${this.trackId}
    `;
  }

  onMouseClick({x, timescale}: TrackMouseEvent): boolean {
    const time = timescale.pxToHpTime(x).toTime('floor');

    const query = `
      select
        id
      from ${this.rootTable}
      where
        track_id = ${this.trackId}
        and ts < ${time}
      order by ts DESC
      limit 1
    `;

    this.engine.query(query).then((result) => {
      const it = result.iter({
        id: NUM,
      });
      if (!it.valid()) {
        return;
      }
      const id = it.id;
      this.trace.selection.selectTrackEvent(this.uri, id);
    });

    return true;
  }

  // We must define this here instead of in `BaseCounterTrack` because
  // `BaseCounterTrack` does not require the query to have an id column. Here,
  // however, we make the assumption that `rootTable` has an id column, as we
  // need it ot make selections in `onMouseClick` above. Whether or not we
  // SHOULD assume `rootTable` has an id column is another matter...
  async getSelectionDetails(id: number): Promise<TrackEventDetails> {
    const query = `
      WITH CTE AS (
        SELECT
          id,
          ts as leftTs
        FROM ${this.rootTable}
        WHERE track_id = ${this.trackId} AND id = ${id}
      )
      SELECT
        *,
        (
          SELECT
            ts
          FROM ${this.rootTable}
          WHERE track_id = ${this.trackId} AND ts > leftTs
          ORDER BY ts ASC
          LIMIT 1
        ) as rightTs
      FROM CTE
    `;

    const counter = await this.engine.query(query);
    const row = counter.iter({
      leftTs: LONG,
      rightTs: LONG_NULL,
    });
    const leftTs = Time.fromRaw(row.leftTs);
    const rightTs = row.rightTs !== null ? Time.fromRaw(row.rightTs) : leftTs;
    const duration = rightTs - leftTs;
    return {ts: leftTs, dur: duration};
  }

  detailsPanel() {
    return new CounterDetailsPanel(this.trace, this.trackId, this.trackName);
  }
}
