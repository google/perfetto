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

import {QueryResult, QuerySlot, SerialTaskQueue} from '../../base/query_slot';
import {duration, Duration, time, Time} from '../../base/time';
import {getTrackUriForTrackId} from '../../components/related_events/utils';
import {Trace} from '../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

export interface ContentionState {
  state: string;
  dur: duration;
  count: number;
}

export interface ContentionBlockedFunction {
  func: string;
  dur: duration;
  count: number;
}

export interface ContentionWaiter {
  threadName: string;
  tid: number | null;
  eventId: number;
}

export interface LockContentionDetails {
  id: number;
  ts: time;
  dur: duration | null;
  monotonicDur: duration | null;
  waiterCount: number;

  lockName: string;

  blockedThreadName: string;
  blockedThreadTid: number | null;
  isBlockedThreadMain: boolean;
  blockedMethod: string;
  blockedSrc: string;

  blockingThreadName: string;
  blockingThreadTid: number | null;
  isBlockingThreadMain: boolean;
  blockingMethod: string;
  blockingSrc: string;

  parentId: number | null; // ID of the contention blocking THIS one, if nested

  blockingTrackUri: string | undefined;

  binderReplyId: number | null;
  blockingBinderTxnId: number | null;
  waiters: ContentionWaiter[];

  threadStates: ContentionState[];
  blockedFunctions: ContentionBlockedFunction[];
}

export class AndroidLockContentionEventSource {
  private readonly queue = new SerialTaskQueue();
  private readonly dataSlot = new QuerySlot<LockContentionDetails | null>(
    this.queue,
  );

  constructor(private readonly trace: Trace) {}

  use(eventId: number): QueryResult<LockContentionDetails | null> {
    return this.dataSlot.use({
      key: eventId,
      queryFn: async () => this.fetchDetails(eventId),
    });
  }

  private async fetchDetails(
    eventId: number,
  ): Promise<LockContentionDetails | null> {
    const mainQuery = await this.trace.engine.query(`
      SELECT 
        id, 
        ts, 
        dur, 
        monotonic_dur, 
        waiter_count,
        lock_name,
        
        blocked_thread_name,
        blocked_thread_tid,
        is_blocked_thread_main,
        short_blocked_method,
        blocked_src,
        
        blocking_thread_name,
        blocking_thread_tid,
        is_blocking_thread_main,
        short_blocking_method,
        blocking_src,
        blocking_utid,
        
        parent_id,
        (SELECT id FROM thread_track WHERE utid = blocking_utid) as blocking_track_id,
        binder_reply_id
      FROM android_monitor_contention_chain
      WHERE id = ${eventId}
    `);

    if (mainQuery.numRows() === 0) return null;

    const row = mainQuery.firstRow({
      id: NUM,
      ts: LONG,
      dur: LONG_NULL,
      monotonic_dur: LONG_NULL,
      waiter_count: NUM_NULL,
      lock_name: STR_NULL,

      blocked_thread_name: STR_NULL,
      blocked_thread_tid: NUM_NULL,
      is_blocked_thread_main: NUM_NULL,
      short_blocked_method: STR_NULL,
      blocked_src: STR_NULL,

      blocking_thread_name: STR_NULL,
      blocking_thread_tid: NUM_NULL,
      is_blocking_thread_main: NUM_NULL,
      short_blocking_method: STR_NULL,
      blocking_src: STR_NULL,
      blocking_utid: NUM_NULL,

      parent_id: NUM_NULL,
      blocking_track_id: NUM_NULL,
      binder_reply_id: NUM_NULL,
    });

    let blockingTrackUri: string | undefined;
    if (row.blocking_track_id !== null) {
      blockingTrackUri = getTrackUriForTrackId(
        this.trace,
        row.blocking_track_id,
      );
    }

    const blockedFunctions: ContentionBlockedFunction[] = [];
    const bfQuery = await this.trace.engine.query(`
      SELECT blocked_function, blocked_function_dur, blocked_function_count
      FROM android_monitor_contention_chain_blocked_functions_by_txn
      WHERE id = ${eventId}
      ORDER BY blocked_function_dur DESC
    `);
    const bfIt = bfQuery.iter({
      blocked_function: STR,
      blocked_function_dur: LONG,
      blocked_function_count: NUM,
    });
    while (bfIt.valid()) {
      blockedFunctions.push({
        func: bfIt.blocked_function,
        dur: Duration.fromRaw(bfIt.blocked_function_dur),
        count: bfIt.blocked_function_count,
      });
      bfIt.next();
    }

    let blockingBinderTxnId: number | null = null;
    if (row.blocking_track_id !== null) {
      const binderQuery = await this.trace.engine.query(`
        SELECT id 
        FROM slice 
        WHERE track_id = ${row.blocking_track_id} 
          AND name = 'binder transaction' 
          AND ts < ${row.ts + (row.dur ?? 0n)} 
          AND (ts + dur) > ${row.ts}
        LIMIT 1
      `);
      if (binderQuery.numRows() > 0) {
        blockingBinderTxnId = binderQuery.firstRow({id: NUM}).id;
      }
    }

    const waiters: ContentionWaiter[] = [];
    const waitersQuery = await this.trace.engine.query(`
      SELECT 
        blocked_thread_name,
        blocked_thread_tid,
        id 
      FROM android_monitor_contention_chain
      WHERE blocking_utid = ${row.blocking_utid}
        AND short_blocking_method = '${row.short_blocking_method || ''}'
        AND ts < ${row.ts + (row.dur ?? 0n)} 
        AND (ts + dur) > ${row.ts}
        AND id != ${eventId}
      ORDER BY ts ASC
    `);
    const waitersIt = waitersQuery.iter({
      blocked_thread_name: STR_NULL,
      blocked_thread_tid: NUM_NULL,
      id: NUM,
    });
    while (waitersIt.valid()) {
      waiters.push({
        threadName: waitersIt.blocked_thread_name || 'Unknown Thread',
        tid: waitersIt.blocked_thread_tid,
        eventId: waitersIt.id,
      });
      waitersIt.next();
    }

    const threadStates: ContentionState[] = [];
    const tsQuery = await this.trace.engine.query(`
      SELECT thread_state, thread_state_dur, thread_state_count
      FROM android_monitor_contention_chain_thread_state_by_txn
      WHERE id = ${eventId}
      ORDER BY thread_state_dur DESC
    `);
    const tsIt = tsQuery.iter({
      thread_state: STR,
      thread_state_dur: LONG,
      thread_state_count: NUM,
    });
    while (tsIt.valid()) {
      threadStates.push({
        state: tsIt.thread_state,
        dur: Duration.fromRaw(tsIt.thread_state_dur),
        count: tsIt.thread_state_count,
      });
      tsIt.next();
    }

    return {
      id: row.id,
      ts: Time.fromRaw(row.ts),
      dur: row.dur !== null ? Duration.fromRaw(row.dur) : null,
      monotonicDur:
        row.monotonic_dur !== null ? Duration.fromRaw(row.monotonic_dur) : null,
      waiterCount: row.waiter_count ?? 0,
      lockName: row.lock_name || 'Unknown Lock',

      blockedThreadName: row.blocked_thread_name || 'Unknown Thread',
      blockedThreadTid: row.blocked_thread_tid,
      isBlockedThreadMain: row.is_blocked_thread_main === 1,
      blockedMethod: row.short_blocked_method || 'Unknown Method',
      blockedSrc: row.blocked_src || 'Unknown Source',

      blockingThreadName: row.blocking_thread_name || 'Unknown Thread',
      blockingThreadTid: row.blocking_thread_tid,
      isBlockingThreadMain: row.is_blocking_thread_main === 1,
      blockingMethod: row.short_blocking_method || 'Unknown Method',
      blockingSrc: row.blocking_src || 'Unknown Source',

      parentId: row.parent_id,
      blockingTrackUri,

      binderReplyId: row.binder_reply_id,
      blockingBinderTxnId,
      waiters,

      threadStates,
      blockedFunctions,
    };
  }
}
