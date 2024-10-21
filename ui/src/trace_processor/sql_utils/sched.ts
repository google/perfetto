// Copyright (C) 2024 The Android Open Source Project
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

import {assertTrue} from '../../base/logging';
import {duration, Time, time} from '../../base/time';
import {Engine} from '../engine';
import {LONG, NUM, NUM_NULL, STR_NULL} from '../query_result';
import {constraintsToQuerySuffix, SQLConstraints} from '../sql_utils';
import {
  asSchedSqlId,
  asThreadStateSqlId,
  asUtid,
  SchedSqlId,
  ThreadStateSqlId,
  Utid,
} from './core_types';
import {getThreadInfo, ThreadInfo} from './thread';
import {getThreadState, getThreadStateFromConstraints} from './thread_state';

// Representation of a single thread state object, corresponding to
// a row for the |thread_slice| table.
export interface Sched {
  // Id into |sched| table.
  id: SchedSqlId;
  // Id of the corresponding entry in the |sched| table.
  threadStateId?: ThreadStateSqlId;
  // Timestamp of the beginning of this thread state in nanoseconds.
  ts: time;
  // Duration of this thread state in nanoseconds.
  dur: duration;
  cpu: number;
  priority: number;
  endState?: string;
  thread: ThreadInfo;
}

export interface SchedWakeupInfo {
  wakeupTs?: time;
  wakerUtid?: Utid;
  wakerCpu?: number;
}

// Gets a list of sched objects from Trace Processor with given
// constraints.
export async function getSchedFromConstraints(
  engine: Engine,
  constraints: SQLConstraints,
): Promise<Sched[]> {
  const query = await engine.query(`
    SELECT
      sched.id as schedSqlId,
      (
        SELECT id
        FROM thread_state
        WHERE
          thread_state.ts = sched.ts
          AND thread_state.utid = sched.utid
      ) as threadStateSqlId,
      sched.ts,
      sched.dur,
      sched.cpu,
      sched.priority as priority,
      sched.end_state as endState,
      sched.utid
    FROM sched
    ${constraintsToQuerySuffix(constraints)}`);
  const it = query.iter({
    schedSqlId: NUM,
    threadStateSqlId: NUM_NULL,
    ts: LONG,
    dur: LONG,
    cpu: NUM,
    priority: NUM,
    endState: STR_NULL,
    utid: NUM,
  });

  const result: Sched[] = [];

  for (; it.valid(); it.next()) {
    result.push({
      id: asSchedSqlId(it.schedSqlId),
      threadStateId: asThreadStateSqlId(it.threadStateSqlId ?? undefined),
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
      priority: it.priority,
      endState: it.endState ?? undefined,
      cpu: it.cpu ?? undefined,
      thread: await getThreadInfo(engine, asUtid(it.utid)),
    });
  }
  return result;
}

export async function getSched(
  engine: Engine,
  id: SchedSqlId,
): Promise<Sched | undefined> {
  const result = await getSchedFromConstraints(engine, {
    filters: [`sched.id=${id}`],
  });
  assertTrue(result.length <= 1);
  if (result.length === 0) {
    return undefined;
  }
  return result[0];
}

// Returns the thread and time of the wakeup that resulted in this running
// sched slice. Omits wakeups that are known to be from interrupt context,
// since we cannot always recover the correct waker cpu with the current
// table layout.
export async function getSchedWakeupInfo(
  engine: Engine,
  sched: Sched,
): Promise<SchedWakeupInfo | undefined> {
  const prevRunnable = await getThreadStateFromConstraints(engine, {
    filters: [
      'state = "R"',
      `ts + dur = ${sched.ts}`,
      `utid = ${sched.thread.utid}`,
      `(irq_context is null or irq_context = 0)`,
    ],
  });
  if (prevRunnable.length === 0 || prevRunnable[0].wakerId === undefined) {
    return undefined;
  }
  const waker = await getThreadState(engine, prevRunnable[0].wakerId);
  if (waker === undefined) {
    return undefined;
  }
  return {
    wakerCpu: waker?.cpu,
    wakerUtid: prevRunnable[0].wakerUtid,
    wakeupTs: prevRunnable[0].ts,
  };
}
