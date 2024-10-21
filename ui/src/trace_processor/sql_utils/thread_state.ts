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
import {Engine} from '../engine';
import {LONG, NUM, NUM_NULL, STR_NULL} from '../query_result';
import {
  constraintsToQuerySuffix,
  fromNumNull,
  SQLConstraints,
} from '../sql_utils';
import {
  asThreadStateSqlId,
  asUtid,
  SchedSqlId,
  ThreadStateSqlId,
  Utid,
} from './core_types';
import {getThreadInfo, ThreadInfo} from './thread';

const states: {[key: string]: string} = {
  'R': 'Runnable',
  'S': 'Sleeping',
  'D': 'Uninterruptible Sleep',
  'T': 'Stopped',
  't': 'Traced',
  'X': 'Exit (Dead)',
  'Z': 'Exit (Zombie)',
  'x': 'Task Dead',
  'I': 'Idle',
  'K': 'Wake Kill',
  'W': 'Waking',
  'P': 'Parked',
  'N': 'No Load',
  '+': '(Preempted)',
};

export function translateState(
  state: string | undefined | null,
  ioWait: boolean | undefined = undefined,
) {
  if (state === undefined) return '';

  // Self describing states
  switch (state) {
    case 'Running':
    case 'Initialized':
    case 'Deferred Ready':
    case 'Transition':
    case 'Stand By':
    case 'Waiting':
      return state;
  }

  if (state === null) {
    return 'Unknown';
  }
  let result = states[state[0]];
  if (ioWait === true) {
    result += ' (IO)';
  } else if (ioWait === false) {
    result += ' (non-IO)';
  }
  for (let i = 1; i < state.length; i++) {
    result += state[i] === '+' ? ' ' : ' + ';
    result += states[state[i]];
  }
  // state is some string we don't know how to translate.
  if (result === undefined) return state;

  return result;
}

// Single thread state slice, corresponding to a row of |thread_slice| table.
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
  // Kernel function where the thread has suspended.
  blockedFunction?: string;
  // Description of the thread itself.
  thread?: ThreadInfo;
  // Thread that was running when this thread was woken up.
  wakerUtid?: Utid;
  // Active thread state at the time of the wakeup.
  wakerId?: ThreadStateSqlId;
  // Was the wakeup from an interrupt context? It is possible for this to be
  // unset even for runnable states, if the trace was recorded without
  // interrupt information.
  wakerInterruptCtx?: boolean;
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
      waker_utid as wakerUtid,
      waker_id as wakerId,
      irq_context as wakerInterruptCtx
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
    wakerId: NUM_NULL,
    wakerInterruptCtx: NUM_NULL,
  });

  const result: ThreadState[] = [];

  for (; it.valid(); it.next()) {
    const ioWait = it.ioWait === null ? undefined : it.ioWait > 0;

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
      wakerUtid: asUtid(it.wakerUtid ?? undefined),
      wakerId: asThreadStateSqlId(it.wakerId ?? undefined),
      wakerInterruptCtx: fromNumNull(it.wakerInterruptCtx) as
        | boolean
        | undefined,
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
