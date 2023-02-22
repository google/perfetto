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

import {Actions} from '../common/actions';
import {EngineProxy} from '../common/engine';
import {NUM, NUM_NULL, STR_NULL} from '../common/query_result';
import {translateState} from '../common/thread_state';
import {fromNs, timeToCode} from '../common/time';

import {copyToClipboard} from './clipboard';
import {globals} from './globals';
import {menuItem} from './popup_menu';
import {scrollToTrackAndTs} from './scroll_helper';
import {
  asUtid,
  SchedSqlId,
  ThreadStateSqlId,
  toTraceTime,
  TPTimestamp,
} from './sql_types';
import {
  constraintsToQueryFragment,
  fromNumNull,
  SQLConstraints,
} from './sql_utils';
import {
  getProcessName,
  getThreadInfo,
  getThreadName,
  ThreadInfo,
} from './thread_and_process_info';
import {dict, Dict, maybeValue, Value, value} from './value';

// Representation of a single thread state object, corresponding to
// a row for the |thread_slice| table.
export interface ThreadState {
  // Id into |thread_state| table.
  threadStateSqlId: ThreadStateSqlId;
  // Id of the corresponding entry in the |sched| table.
  schedSqlId?: SchedSqlId;
  // Timestamp of the the beginning of this thread state in nanoseconds.
  ts: TPTimestamp;
  // Duration of this thread state in nanoseconds.
  dur: number;
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
    engine: EngineProxy, constraints: SQLConstraints): Promise<ThreadState[]> {
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
    ${constraintsToQueryFragment(constraints)}`);
  const it = query.iter({
    threadStateSqlId: NUM,
    schedSqlId: NUM_NULL,
    ts: NUM,
    dur: NUM,
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
    const wakerUtid = asUtid(it.wakerUtid || undefined);

    // TODO(altimin): Consider fetcing thread / process info using a single
    // query instead of one per row.
    result.push({
      threadStateSqlId: it.threadStateSqlId as ThreadStateSqlId,
      schedSqlId: fromNumNull(it.schedSqlId) as (SchedSqlId | undefined),
      ts: it.ts as TPTimestamp,
      dur: it.dur,
      cpu: fromNumNull(it.cpu),
      state: translateState(it.state || undefined, ioWait),
      blockedFunction: it.blockedFunction || undefined,
      thread: await getThreadInfo(engine, asUtid(it.utid)),
      wakerThread: wakerUtid ? await getThreadInfo(engine, wakerUtid) :
                               undefined,
    });
  }
  return result;
}

export async function getThreadState(
    engine: EngineProxy, id: number): Promise<ThreadState|undefined> {
  const result = await getThreadStateFromConstraints(engine, {
    where: [`id=${id}`],
  });
  if (result.length > 1) {
    throw new Error(`thread_state table has more than one row with id ${id}`);
  }
  if (result.length === 0) {
    return undefined;
  }
  return result[0];
}

export function goToSchedSlice(cpu: number, id: SchedSqlId, ts: TPTimestamp) {
  let trackId: string|undefined;
  for (const track of Object.values(globals.state.tracks)) {
    if (track.kind === 'CpuSliceTrack' &&
        (track.config as {cpu: number}).cpu === cpu) {
      trackId = track.id;
    }
  }
  if (trackId === undefined) {
    return;
  }
  globals.makeSelection(Actions.selectSlice({id, trackId}));
  scrollToTrackAndTs(trackId, ts);
}

function stateToValue(
    state: string,
    cpu: number|undefined,
    id: SchedSqlId|undefined,
    ts: TPTimestamp): Value|null {
  if (!state) {
    return null;
  }
  if (id === undefined || cpu === undefined) {
    return value(state);
  }
  return value(`${state} on CPU ${cpu}`, {
    rightButton: {
      action: () => {
        goToSchedSlice(cpu, id, ts);
      },
      hoverText: 'Go to CPU slice',
    },
  });
}

export function threadStateToDict(state: ThreadState): Dict {
  const result: {[name: string]: Value|null} = {};

  result['Start time'] = value(timeToCode(toTraceTime(state.ts)));
  result['Duration'] = value(timeToCode(fromNs(state.dur)));
  result['State'] =
      stateToValue(state.state, state.cpu, state.schedSqlId, state.ts);
  result['Blocked function'] = maybeValue(state.blockedFunction);
  const process = state?.thread?.process;
  result['Process'] = maybeValue(process ? getProcessName(process) : undefined);
  const thread = state?.thread;
  result['Thread'] = maybeValue(thread ? getThreadName(thread) : undefined);
  if (state.wakerThread) {
    const process = state.wakerThread.process;
    result['Waker'] = dict({
      'Process': maybeValue(process ? getProcessName(process) : undefined),
      'Thread': maybeValue(getThreadName(state.wakerThread)),
    });
  }
  result['SQL id'] = value(`thread_state[${state.threadStateSqlId}]`, {
    contextMenu: [
      menuItem(
          'Copy SQL query',
          () => {
            copyToClipboard(`select * from thread_state where id=${
                state.threadStateSqlId}`);
          }),
    ],
  });

  return dict(result);
}
