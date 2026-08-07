// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {Time, type time} from '../../base/time';
import type {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR_NULL} from '../../trace_processor/query_result';
import {asSliceSqlId, type SliceSqlId} from '../sql_utils/core_types';

export interface FlowRow {
  readonly id: number;
  readonly sliceId: SliceSqlId;
  readonly sliceName: string;
  readonly sliceChromeCustomName?: string;
  readonly sliceStartTs: time;
  readonly sliceEndTs: time;
  readonly threadName: string;
  readonly processName: string;
}

export interface DirectlyConnectedFlows {
  readonly preceding: readonly FlowRow[];
  readonly following: readonly FlowRow[];
}

export async function getConnectedFlows(
  engine: Engine,
  sliceId: number,
): Promise<DirectlyConnectedFlows> {
  const query = `
    INCLUDE PERFETTO MODULE slices.flow;

    select
      f.id as id,
      f.slice_out as sliceOut,
      f.slice_in as sliceIn,
      t.id as otherSliceId,
      t.name as otherSliceName,
      t.ts as otherSliceStartTs,
      (t.ts + t.dur) as otherSliceEndTs,
      (thread.name || ' ' || thread.tid) as otherThreadName,
      (process.name || ' ' || process.pid) as otherProcessName
    from directly_connected_flow(${sliceId}) f
    join slice t on (case when f.slice_in = ${sliceId} then f.slice_out else f.slice_in end) = t.id
    left join thread_track track on track.id = t.track_id
    left join thread using (utid)
    left join process using (upid)
  `;

  const result = await engine.query(query);
  const preceding: FlowRow[] = [];
  const following: FlowRow[] = [];

  const it = result.iter({
    id: NUM,
    sliceOut: NUM,
    sliceIn: NUM,
    otherSliceId: NUM,
    otherSliceName: STR_NULL,
    otherSliceStartTs: LONG,
    otherSliceEndTs: LONG,
    otherThreadName: STR_NULL,
    otherProcessName: STR_NULL,
  });

  const nullToStr = (s: null | string): string => (s === null ? 'NULL' : s);

  for (; it.valid(); it.next()) {
    const row: FlowRow = {
      id: it.id,
      sliceId: asSliceSqlId(it.otherSliceId),
      sliceName: nullToStr(it.otherSliceName),
      sliceStartTs: Time.fromRaw(it.otherSliceStartTs),
      sliceEndTs: Time.fromRaw(it.otherSliceEndTs),
      threadName: nullToStr(it.otherThreadName),
      processName: nullToStr(it.otherProcessName),
    };

    if (it.sliceIn === sliceId) {
      preceding.push(row);
    } else {
      following.push(row);
    }
  }

  return {preceding, following};
}
