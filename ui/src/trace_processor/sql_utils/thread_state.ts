// Copyright (C) 2023 The Android Open Source Project
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

import {duration, Time, time} from '../../base/time';
import {exists} from '../../base/utils';
import {translateState} from '../../common/thread_state';
import {Engine} from '../engine';
import {LONG, NUM, NUM_NULL, STR_NULL} from '../query_result';
import {
  constraintsToQuerySuffix,
  fromNumNull,
  SQLConstraints,
} from '../sql_utils';

import {globals} from '../../frontend/globals';
import {scrollToTrackAndTs} from '../../frontend/scroll_helper';
import {asUtid, SchedSqlId, ThreadStateSqlId} from './core_types';
import {CPU_SLICE_TRACK_KIND} from '../../core/track_kinds';
import {getThreadInfo, ThreadInfo} from './thread';

// Representation of a single thread state object, corresponding to
// a row for the |thread_slice| table.
export interface ThreadState {
  // Id into |thread_state| table.
  threadStateSqlId: ThreadStateSqlId;
  // Id of the corresponding entry in the |sched| table.
  schedSqlId?: SchedSqlId;
  // Timestamp of the beginning of this thread state in nanoseconds.
  ts: time;
  // Duration of this thread state in nanoseconds.
  dur: duration;
  // CPU id if this thread state corresponds to a thread running on the CPU.
  cpu?: number;
  // Human-readable name of this thread state.
  state: string;
  blockedFunction?: string;

  thread?: ThreadInfo;
  wakerThread?: ThreadInfo;
}

// Gets a list of thread state objects from Trace Processor with given
// constraints.
export async function getThreadStateFromConstraints(
  engine: Engine,
  constraints: SQLConstraints,
): Promise<ThreadState[]> {
  const query = await engine.query(`
    SELECT
      thread_state.id as threadStateSqlId,
      (select sched.id
        from sched
        where sched.ts=thread_state.ts and sched.utid=thread_state.utid
        limit 1
       ) as schedSqlId,
      ts,
      thread_state.dur as dur,
      thread_state.cpu as cpu,
      state,
      thread_state.blocked_function as blockedFunction,
      io_wait as ioWait,
      thread_state.utid as utid,
      waker_utid as wakerUtid
    FROM thread_state
    ${constraintsToQuerySuffix(constraints)}`);
  const it = query.iter({
    threadStateSqlId: NUM,
    schedSqlId: NUM_NULL,
    ts: LONG,
    dur: LONG,
    cpu: NUM_NULL,
    state: STR_NULL,
    blockedFunction: STR_NULL,
    ioWait: NUM_NULL,
    utid: NUM,
    wakerUtid: NUM_NULL,
  });

  const result: ThreadState[] = [];

  for (; it.valid(); it.next()) {
    const ioWait = it.ioWait === null ? undefined : it.ioWait > 0;
    const wakerUtid = asUtid(it.wakerUtid ?? undefined);

    // TODO(altimin): Consider fetcing thread / process info using a single
    // query instead of one per row.
    result.push({
      threadStateSqlId: it.threadStateSqlId as ThreadStateSqlId,
      schedSqlId: fromNumNull(it.schedSqlId) as SchedSqlId | undefined,
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
      cpu: fromNumNull(it.cpu),
      state: translateState(it.state ?? undefined, ioWait),
      blockedFunction: it.blockedFunction ?? undefined,
      thread: await getThreadInfo(engine, asUtid(it.utid)),
      wakerThread: wakerUtid
        ? await getThreadInfo(engine, wakerUtid)
        : undefined,
    });
  }
  return result;
}

export async function getThreadState(
  engine: Engine,
  id: number,
): Promise<ThreadState | undefined> {
  const result = await getThreadStateFromConstraints(engine, {
    filters: [`id=${id}`],
  });
  if (result.length > 1) {
    throw new Error(`thread_state table has more than one row with id ${id}`);
  }
  if (result.length === 0) {
    return undefined;
  }
  return result[0];
}

export function goToSchedSlice(cpu: number, id: SchedSqlId, ts: time) {
  let trackId: string | undefined;
  for (const track of Object.values(globals.state.tracks)) {
    if (exists(track?.uri)) {
      const trackInfo = globals.trackManager.resolveTrackInfo(track.uri);
      if (trackInfo?.tags?.kind === CPU_SLICE_TRACK_KIND) {
        if (trackInfo?.tags?.cpu === cpu) {
          trackId = track.key;
          break;
        }
      }
    }
  }
  if (trackId === undefined) {
    return;
  }
  globals.setLegacySelection(
    {
      kind: 'SCHED_SLICE',
      id,
      trackKey: trackId,
    },
    {
      clearSearch: true,
      pendingScrollId: undefined,
      switchToCurrentSelectionTab: true,
    },
  );

  scrollToTrackAndTs(trackId, ts);
}
