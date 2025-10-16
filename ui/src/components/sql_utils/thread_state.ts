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
import {Engine} from '../../trace_processor/engine';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {
  constraintsToQuerySuffix,
  fromNumNull,
  SQLConstraints,
} from '../../trace_processor/sql_utils';
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
    case 'Created':
    case 'Running':
    case 'Initialized':
    case 'DeferredReady':
    case 'Transition':
    case 'Standby':
    case 'Waiting':
    case 'Ready':
    case 'Terminated':
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
  id: ThreadStateSqlId;
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
  // Kernel priority of this thread state.
  priority?: number;
}

// Gets a list of thread state objects from Trace Processor with given
// constraints.
export async function getThreadStateFromConstraints(
  engine: Engine,
  constraints: SQLConstraints,
): Promise<ThreadState[]> {
  const query = await engine.query(`
    WITH raw AS (
      SELECT
      ts.id,
      sched.id AS sched_id,
      ts.ts,
      ts.dur,
      ts.cpu,
      ts.state,
      ts.blocked_function,
      ts.io_wait,
      ts.utid,
      ts.waker_utid,
      ts.waker_id,
      ts.irq_context,
      sched.priority
    FROM thread_state ts
    LEFT JOIN sched USING (utid, ts)
    )
    SELECT * FROM raw

    ${constraintsToQuerySuffix(constraints)}`);
  const it = query.iter({
    id: NUM,
    sched_id: NUM_NULL,
    ts: LONG,
    dur: LONG,
    cpu: NUM_NULL,
    state: STR_NULL,
    blocked_function: STR_NULL,
    io_wait: NUM_NULL,
    utid: NUM,
    waker_utid: NUM_NULL,
    waker_id: NUM_NULL,
    irq_context: NUM_NULL,
    priority: NUM_NULL,
  });

  const result: ThreadState[] = [];

  for (; it.valid(); it.next()) {
    const ioWait = it.io_wait === null ? undefined : it.io_wait > 0;

    // TODO(altimin): Consider fetching thread / process info using a single
    // query instead of one per row.
    result.push({
      id: it.id as ThreadStateSqlId,
      schedSqlId: fromNumNull(it.sched_id) as SchedSqlId | undefined,
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
      cpu: fromNumNull(it.cpu),
      state: translateState(it.state ?? undefined, ioWait),
      blockedFunction: it.blocked_function ?? undefined,
      thread: await getThreadInfo(engine, asUtid(it.utid)),
      wakerUtid: asUtid(it.waker_utid ?? undefined),
      wakerId: asThreadStateSqlId(it.waker_id ?? undefined),
      wakerInterruptCtx: fromNumNull(it.irq_context) as boolean | undefined,
      priority: fromNumNull(it.priority),
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
