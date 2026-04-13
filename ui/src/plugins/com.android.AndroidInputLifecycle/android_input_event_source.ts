// Copyright (C) 2026 The Android Open Source Project
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

import {Trace} from '../../public/trace';
import {
  LONG_NULL,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {duration, time, Time, Duration} from '../../base/time';
import {
  getTrackUriForTrackId,
  enrichDepths,
} from '../../components/related_events/utils';
import {QuerySlot, QueryResult, SerialTaskQueue} from '../../base/query_slot';

export interface NavTarget {
  id: number;
  trackUri: string;
  ts: time;
  dur: duration;
  depth: number;
}

export interface InputChainRow {
  uiRowId: string;
  channel: string;
  totalLatency: duration | null;

  durReader: duration | null;
  deltaDispatch: duration | null;
  deltaReceive: duration | null;
  deltaConsume: duration | null;
  deltaFrame: duration | null;

  navReader?: NavTarget;
  navDispatch?: NavTarget;
  navConsume?: NavTarget;
  navReceive?: NavTarget;
  navFrame?: NavTarget;

  allTrackUris: string[];
}

export class AndroidInputEventSource {
  private readonly queue = new SerialTaskQueue();
  private readonly dataSlot = new QuerySlot<InputChainRow[]>(this.queue);

  constructor(private readonly trace: Trace) {}

  use(sliceId: number): QueryResult<InputChainRow[]> {
    return this.dataSlot.use({
      key: {sliceId},
      queryFn: async () => {
        const rows = await this.fetchRows(sliceId);
        await this.enrichAllDepths(rows);
        return rows;
      },
    });
  }

  private async fetchRows(sliceId: number): Promise<InputChainRow[]> {
    const result = await this.trace.engine.query(
      `SELECT * FROM _android_input_lifecycle_by_slice_id(${sliceId})`,
    );

    const rows: InputChainRow[] = [];
    let index = 0;

    const it = result.iter({
      input_id: STR_NULL,
      channel: STR_NULL,
      total_latency: LONG_NULL,

      ts_reader: LONG_NULL,
      id_reader: NUM_NULL,
      track_reader: NUM_NULL,
      dur_reader: LONG_NULL,

      ts_dispatch: LONG_NULL,
      id_dispatch: NUM_NULL,
      track_dispatch: NUM_NULL,
      dur_dispatch: LONG_NULL,

      ts_receive: LONG_NULL,
      id_receive: NUM_NULL,
      track_receive: NUM_NULL,
      dur_receive: LONG_NULL,

      ts_consume: LONG_NULL,
      id_consume: NUM_NULL,
      track_consume: NUM_NULL,
      dur_consume: LONG_NULL,

      ts_frame: LONG_NULL,
      id_frame: NUM_NULL,
      track_frame: NUM_NULL,
      dur_frame: LONG_NULL,
    });

    while (it.valid()) {
      const navReader = this.makeNav(
        it.id_reader,
        it.track_reader,
        it.ts_reader,
        it.dur_reader,
      );
      const navDispatch = this.makeNav(
        it.id_dispatch,
        it.track_dispatch,
        it.ts_dispatch,
        it.dur_dispatch,
      );
      const navReceive = this.makeNav(
        it.id_receive,
        it.track_receive,
        it.ts_receive,
        it.dur_receive,
      );
      const navConsume = this.makeNav(
        it.id_consume,
        it.track_consume,
        it.ts_consume,
        it.dur_consume,
      );
      const navFrame = this.makeNav(
        it.id_frame,
        it.track_frame,
        it.ts_frame,
        it.dur_frame,
      );

      const allTrackUris: string[] = [];
      for (const nav of [
        navReader,
        navDispatch,
        navReceive,
        navConsume,
        navFrame,
      ]) {
        if (nav) allTrackUris.push(nav.trackUri);
      }

      rows.push({
        uiRowId: `row-${index++}`,
        channel: it.channel ?? '',
        totalLatency:
          it.total_latency !== null ? Duration.fromRaw(it.total_latency) : null,
        durReader:
          it.dur_reader !== null ? Duration.fromRaw(it.dur_reader) : null,
        deltaDispatch:
          it.ts_dispatch !== null && it.ts_reader !== null
            ? Duration.fromRaw(it.ts_dispatch - it.ts_reader)
            : null,
        deltaReceive:
          it.ts_receive !== null && it.ts_dispatch !== null
            ? Duration.fromRaw(it.ts_receive - it.ts_dispatch)
            : null,
        deltaConsume:
          it.ts_consume !== null && it.ts_receive !== null
            ? Duration.fromRaw(it.ts_consume - it.ts_receive)
            : null,
        deltaFrame:
          it.ts_frame !== null && it.ts_consume !== null
            ? Duration.fromRaw(it.ts_frame - it.ts_consume)
            : null,
        navReader,
        navDispatch,
        navReceive,
        navConsume,
        navFrame,
        allTrackUris,
      });

      it.next();
    }

    return rows;
  }

  private async enrichAllDepths(rows: InputChainRow[]) {
    const targets: NavTarget[] = [];
    for (const row of rows) {
      for (const nav of [
        row.navReader,
        row.navDispatch,
        row.navReceive,
        row.navConsume,
        row.navFrame,
      ]) {
        if (nav) targets.push(nav);
      }
    }
    if (targets.length === 0) return;
    await enrichDepths(this.trace, targets);
  }

  private makeNav(
    id: number | null,
    trackId: number | null,
    ts: bigint | null,
    dur: bigint | null,
  ): NavTarget | undefined {
    if (id === null || trackId === null || ts === null) return undefined;
    return {
      id,
      trackUri: getTrackUriForTrackId(this.trace, trackId),
      ts: Time.fromRaw(ts),
      dur: Duration.fromRaw(dur ?? 0n),
      depth: 0,
    };
  }
}
